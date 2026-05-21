/***********************************************
 *
 * @File:    terminal.c
 * @Purpose: Main terminal loop using select() to multiplex user commands and incoming network connections.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#include "terminal.h"
#include "envoy.h"
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>

/* ── Connection thread ────────────────────────────────────────────────────
 * Each accepted incoming connection is handled in a detached pthread so
 * that long file transfers (sigil receive, list send, order receive) do
 * not block the select() loop.
 *
 * Using a THREAD (not fork) is essential: threads share the process
 * address space, so pledge table and product array updates made inside
 * handle_incoming are immediately visible to the main thread.
 * A forked child would have its own copy — pledge updates would be lost.
 * ─────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* Accepted socket for this peer. */
    int          client_fd;
    /* Shared Maester configuration. */
    Maester     *maester;
    /* Shared inventory pointer. */
    Product    **products;
    /* Shared inventory count. */
    int         *total_products;
} ConnArgs;

/* Active connection-thread counter. */
static pthread_mutex_t g_conn_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_conn_cond    = PTHREAD_COND_INITIALIZER;
static int             g_conn_active  = 0;

/* Self-pipe for prompt notification.
 * Async threads (envoys, connection threads) write a byte here after
 * printing output.  select() wakes up, we set nNeedPrompt = 1, and
 * the '$ ' is reprinted on the next loop iteration. */
static int g_notify_pipe[2] = {-1, -1};

void notify_prompt(void) {
    if (g_notify_pipe[1] >= 0) {
        /* Any byte is enough; the main select() loop only needs a wake-up. */
        write(g_notify_pipe[1], ".", 1);
    }
}

static void *handle_connection_thread(void *pArg) {
    ConnArgs *pstA = (ConnArgs *)pArg;
    Frame     stFrame;
    memset(&stFrame, 0, sizeof(stFrame));

    /* Each connection starts with one protocol frame. Some handlers then
     * continue reading more frames on the same socket for file transfers. */
    if (0 == recv_frame(pstA->client_fd, &stFrame)) {
        if (!validate_frame(&stFrame)) {
            char sNackIp[IP_SIZE];
            int  nNackPort;
            if (0 == parse_origin(stFrame.origin, sNackIp, &nNackPort)) {
                send_nack(sNackIp, nNackPort, pstA->maester);
            }
        } else {
            /* The dispatcher chooses pledge/list/order/disconnect handling. */
            handle_incoming(&stFrame, pstA->client_fd, pstA->maester,
                            pstA->products, pstA->total_products);
            /* Async output is done; ask the main loop to reprint the prompt. */
            notify_prompt();
        }
    }

    /* The thread owns the accepted socket and its ConnArgs allocation. */
    close(pstA->client_fd);
    free(pstA);

    /* Signal that this connection thread has finished */
    pthread_mutex_lock(&g_conn_mutex);
    g_conn_active--;
    pthread_cond_signal(&g_conn_cond);
    pthread_mutex_unlock(&g_conn_mutex);

    return NULL;
}

/********************
 *
 * @Name: terminal
 * @Def: Main terminal interface. Uses select() to multiplex stdin (user
 *       commands) and the server socket (incoming Maester messages).
 *
 *       Incoming connections are handled in detached pthreads so that
 *       blocking file-transfer operations do not stall the prompt.
 *
 *       Outgoing missions (PLEDGE, LIST PRODUCTS, START TRADE) are
 *       delegated to Envoy child processes.
 *
 * @Arg: In/Out: pnTotalProducts = number of products in the local inventory
 *       In/Out: ppstProducts    = local product array (may be realloced by trade)
 *       In:     pstMaester      = pointer to the local Maester configuration
 *       In:     nServerFd       = listening server socket fd
 *       In:     pnStopFlag      = pointer to SIGINT stop flag
 * @Ret: None
 *
 ********************/
void terminal(int *pnTotalProducts, Product **ppstProducts, Maester *pstMaester,
              int nServerFd, volatile sig_atomic_t *pnStopFlag) {
    /* EXIT command requested shutdown. */
    int              nExitFlag   = 0;
    /* Prompt must be printed next loop. */
    int              nNeedPrompt = 1;
    char            *psRealm     = NULL;
    char            *psSigil     = NULL;
    fd_set           stFds;
    struct timeval   stTv;
    RemoteInventory *pstCache    = NULL;
    int              nCacheCount = 0;
    /* Terminal is inside trade sub-mode. */
    int              nInTrade    = 0;
    /* Reserved envoy for current trade. */
    int              nTradeEnvoy = -1;
    TradeSession     stTradeSess;
    memset(&stTradeSess, 0, sizeof(stTradeSess));

    /* Initialise envoy pool */
    init_envoys(pstMaester->envoy_count, pstMaester, ppstProducts,
                pnTotalProducts, &pstCache, &nCacheCount);

    /* Create the self-pipe used by async threads to wake select() */
    pipe(g_notify_pipe);

    while (!*pnStopFlag && !nExitFlag) {

        if (nNeedPrompt) {
            if (nInTrade) {
                printF("(trade)> ");
            } else {
                printF("$ ");
            }
            nNeedPrompt = 0;
        }

        /* Build the fd_set for one select() call. */
        FD_ZERO(&stFds);
        FD_SET(STDIN_FILENO, &stFds);
        FD_SET(nServerFd, &stFds);
        if (g_notify_pipe[0] >= 0) {
            FD_SET(g_notify_pipe[0], &stFds);
        }
        {
            int ei;
            for (ei = 0; ei < pstMaester->envoy_count; ei++) {
                int nEfd = get_envoy_pipe_fd(ei);
                if (nEfd >= 0) {
                    FD_SET(nEfd, &stFds);
                }
            }
        }
        stTv.tv_sec  = 1;
        stTv.tv_usec = 0;

        {
            /* Highest fd, required by select(). */
            int nNfds  = nServerFd;
            int nReady;
            int ei;

            if (g_notify_pipe[0] > nNfds) {
                nNfds = g_notify_pipe[0];
            }
            for (ei = 0; ei < pstMaester->envoy_count; ei++) {
                int nEfd = get_envoy_pipe_fd(ei);
                if (nEfd > nNfds) {
                    nNfds = nEfd;
                }
            }
            /* select() lets one thread react to keyboard, network, Envoys,
             * prompt notifications, and periodic timeout checks. */
            nReady = select(nNfds + 1, &stFds, NULL, NULL, &stTv);

            /* These checks run every loop, even when no fd becomes ready. */
            check_pledge_timeouts();
            check_pledge_envoys();

            if (nReady < 0) {
                /* A signal interrupted select(), so retry the loop. */
                if (EINTR == errno) {
                    continue;
                }
                break;
            }

            /* ── Incoming network message ───────────────────────────────── */
            if (FD_ISSET(nServerFd, &stFds)) {
                int nClientFd = accept(nServerFd, NULL, NULL);
                if (nClientFd >= 0) {
                    set_socket_timeout(nClientFd, 30);
                    /* Pack shared pointers for the connection thread. */
                    ConnArgs *pstArgs = malloc(sizeof(ConnArgs));
                    if (NULL != pstArgs) {
                        pthread_t stT;
                        pstArgs->client_fd      = nClientFd;
                        pstArgs->maester        = pstMaester;
                        pstArgs->products       = ppstProducts;
                        pstArgs->total_products = pnTotalProducts;
                        /* Count active threads so shutdown waits safely. */
                        pthread_mutex_lock(&g_conn_mutex);
                        g_conn_active++;
                        pthread_mutex_unlock(&g_conn_mutex);
                        if (0 == pthread_create(&stT, NULL,
                                               handle_connection_thread,
                                               pstArgs)) {
                            pthread_detach(stT);
                        } else {
                            pthread_mutex_lock(&g_conn_mutex);
                            g_conn_active--;
                            pthread_mutex_unlock(&g_conn_mutex);
                            close(nClientFd);
                            free(pstArgs);
                        }
                    } else {
                        close(nClientFd);
                    }
                }
                nNeedPrompt = 1;
            }

            /* ── Async thread notification — reprint prompt ─────────────── */
            if (g_notify_pipe[0] >= 0 && FD_ISSET(g_notify_pipe[0], &stFds)) {
                char sDrain[16];
                /* Drain bytes written by notify_prompt(); content is ignored. */
                read(g_notify_pipe[0], sDrain, sizeof(sDrain));
                nNeedPrompt = 1;
            }

            /* ── Envoy pipe results ──────────────────────────────────────── */
            for (ei = 0; ei < pstMaester->envoy_count; ei++) {
                int nEfd = get_envoy_pipe_fd(ei);
                if (nEfd >= 0 && FD_ISSET(nEfd, &stFds)) {
                    /* Child finished; read pipe payload and mutate parent state. */
                    apply_envoy_result(ei);
                }
            }
        }

        /* ── User command from stdin ─────────────────────────────────────── */
        if (FD_ISSET(STDIN_FILENO, &stFds)) {
            if (nInTrade) {
                /* In trade mode, input is interpreted by trade_session_input(). */
                char *psTradeLine = read_screen();
                if (NULL != psTradeLine) {
                    int        nTr;
                    TradeItem *pstItems;
                    int        nItems;
                    char       sTradeRealm[50];
                    to_upper(psTradeLine);
                    /* trade_session_input() returns:
                     *   0  = still collecting items, stay in trade mode
                     *   1  = user typed SEND, ready to dispatch
                     *  -1  = user typed CANCEL or input error, abort */
                    nTr = trade_session_input(&stTradeSess, psTradeLine);
                    free(psTradeLine);
                    if (1 == nTr) {
                        strncpy(sTradeRealm, stTradeSess.sRealm, 49);
                        sTradeRealm[49]      = '\0';
                        pstItems             = stTradeSess.pstItems;
                        nItems               = stTradeSess.nCount;
                        /* Transfer ownership of the items array to dispatch_trade;
                         * clear the session's pointer so trade_session_free()
                         * does not double-free the same allocation. */
                        stTradeSess.pstItems = NULL;
                        stTradeSess.nCount   = 0;
                        /* SEND transfers the selected items to the Envoy child. */
                        dispatch_trade(nTradeEnvoy, sTradeRealm, pstItems, nItems);
                        trade_session_free(&stTradeSess);
                        nInTrade    = 0;
                        nTradeEnvoy = -1;
                    } else if (-1 == nTr) {
                        /* CANCEL: release the reserved Envoy slot so it can be
                         * reused for future missions. */
                        release_envoy(nTradeEnvoy);
                        trade_session_free(&stTradeSess);
                        nInTrade    = 0;
                        nTradeEnvoy = -1;
                    }
                } else {
                    /* EOF on stdin during trade — cancel cleanly */
                    release_envoy(nTradeEnvoy);
                    trade_session_free(&stTradeSess);
                    nInTrade    = 0;
                    nTradeEnvoy = -1;
                }
            } else {
            int nEnvoyIdx;

            /* Normal mode: parse a top-level command from the prompt. */
            switch (parse_command(&psRealm, &psSigil)) {

                case CMD_LIST_REALMS:
                    list_realms(*pstMaester);
                    break;

                case CMD_LIST_PRODUCTS_OWN: {
                    Product *pstSnap  = NULL;
                    int      nSnapCnt = 0;
                    /* Snapshot inventory so printing does not hold the mutex. */
                    pthread_mutex_lock(&g_data_mutex);
                    nSnapCnt = *pnTotalProducts;
                    if (nSnapCnt > 0) {
                        pstSnap = malloc((size_t)nSnapCnt * sizeof(Product));
                        if (NULL != pstSnap) {
                            memcpy(pstSnap, *ppstProducts,
                                   (size_t)nSnapCnt * sizeof(Product));
                        }
                    }
                    pthread_mutex_unlock(&g_data_mutex);
                    if (NULL != pstSnap) {
                        list_products(nSnapCnt, pstSnap);
                        free(pstSnap);
                    } else if (0 == nSnapCnt) {
                        list_products(0, NULL);
                    }
                    break;
                }

                case CMD_LIST_PRODUCTS_REALM:
                    /* Outgoing network work is delegated to a free Envoy. */
                    nEnvoyIdx = find_free_envoy();
                    if (nEnvoyIdx < 0) {
                        printF("All envoys are occupied. Your command must wait.\n");
                    } else {
                        dispatch_list_products(nEnvoyIdx, psRealm);
                    }
                    free(psRealm); psRealm = NULL;
                    break;

                case CMD_START_TRADE:
                    nEnvoyIdx = find_free_envoy();
                    if (nEnvoyIdx < 0) {
                        printF("All envoys are occupied. Your command must wait.\n");
                        free(psRealm); psRealm = NULL;
                        break;
                    }
                    /* Reserve now; the child is forked only after SEND. */
                    reserve_envoy(nEnvoyIdx, ENVOY_TRADE, psRealm);
                    if (0 == trade_session_init(&stTradeSess, psRealm,
                                                &pstCache, &nCacheCount)) {
                        nInTrade    = 1;
                        nTradeEnvoy = nEnvoyIdx;
                    } else {
                        release_envoy(nEnvoyIdx);
                    }
                    free(psRealm); psRealm = NULL;
                    break;

                case CMD_PLEDGE: {
                    int    nTestFd;
                    size_t nDL;
                    /* Check sigil ownership and existence before forking */
                    if (NULL != pstMaester->user_dir) {
                        nDL = strlen(pstMaester->user_dir);
                        if (0 != strncmp(psSigil, pstMaester->user_dir, nDL) ||
                            (psSigil[nDL] != '/' && psSigil[nDL] != '\\')) {
                            printF("Honour demands your own house sigil. The pledge is hereby withdrawn.\n");
                            free(psRealm); psRealm = NULL;
                            free(psSigil); psSigil = NULL;
                            break;
                        }
                    }
                    /* Basic local validation before spending an Envoy slot. */
                    nTestFd = open(psSigil, O_RDONLY);
                    if (-1 == nTestFd) {
                        printF("Sigil not found. The pledge is hereby withdrawn.\n");
                        free(psRealm); psRealm = NULL;
                        free(psSigil); psSigil = NULL;
                        break;
                    }
                    close(nTestFd);
                    nEnvoyIdx = find_free_envoy();
                    if (nEnvoyIdx < 0) {
                        printF("All envoys are occupied. Your command must wait.\n");
                    } else {
                        dispatch_pledge(nEnvoyIdx, psRealm, psSigil);
                    }
                    free(psRealm); psRealm = NULL;
                    free(psSigil); psSigil = NULL;
                    break;
                }

                case CMD_PLEDGE_RESPOND_ACCEPT:
                    nEnvoyIdx = find_free_envoy();
                    if (nEnvoyIdx < 0) {
                        printF("All envoys are occupied. Cannot respond now.\n");
                    } else {
                        dispatch_pledge_respond(nEnvoyIdx, psRealm, "ACCEPT");
                    }
                    free(psRealm); psRealm = NULL;
                    break;

                case CMD_PLEDGE_RESPOND_REJECT:
                    nEnvoyIdx = find_free_envoy();
                    if (nEnvoyIdx < 0) {
                        printF("All envoys are occupied. Cannot respond now.\n");
                    } else {
                        dispatch_pledge_respond(nEnvoyIdx, psRealm, "REJECT");
                    }
                    free(psRealm); psRealm = NULL;
                    break;

                case CMD_PLEDGE_STATUS:
                    show_pledge_status();
                    break;

                case CMD_ENVOY_STATUS:
                    show_envoy_status();
                    break;

                case CMD_EXIT:
                    exit_maester(*pstMaester);
                    nExitFlag = 1;
                    break;

                case CMD_UNKNOWN:
                    printF("Unknown command.\n");
                    break;

                case CMD_INCOMPLETE_LIST:
                    printF("Incomplete command: LIST requires additional arguments. (LIST REALMS or LIST PRODUCTS)\n");
                    break;

                case CMD_INCOMPLETE_PLEDGE:
                    printF("Incomplete command: PLEDGE requires additional arguments. (PLEDGE <REALM> <sigil> or PLEDGE STATUS or PLEDGE RESPOND <REALM> ACCEPT/REJECT)\n");
                    break;

                case CMD_INCOMPLETE_PLEDGE_RESPOND:
                    printF("Incomplete command: PLEDGE RESPOND requires additional arguments. (PLEDGE RESPOND <REALM> ACCEPT/REJECT)\n");
                    break;

                case CMD_INCOMPLETE_ALLIANCE:
                    printF("Incomplete command: PLEDGE requires a realm and sigil. (PLEDGE <REALM> <sigil.jpg>)\n");
                    break;

                case CMD_INCOMPLETE_TRADE:
                    printF("Missing arguments, can't start a trade. Please review the syntax.\n");
                    break;

                case CMD_INCOMPLETE_ENVOY:
                    printF("Incomplete command: ENVOY requires additional arguments. (ENVOY STATUS)\n");
                    break;

                default:
                    break;
            }

            if (NULL != psRealm) {
                free(psRealm);
                psRealm = NULL;
            }
            if (NULL != psSigil) {
                free(psSigil);
                psSigil = NULL;
            }
            } /* end else (!nInTrade) */
            nNeedPrompt = 1;
        }
    }

    /* Block until every connection thread has returned.  Threads share
     * ppstProducts and pstMaester directly; freeing those before a thread
     * finishes would be a use-after-free.  The condition variable is
     * signalled by handle_connection_thread() just before it returns. */
    pthread_mutex_lock(&g_conn_mutex);
    while (g_conn_active > 0) {
        pthread_cond_wait(&g_conn_cond, &g_conn_mutex);
    }
    pthread_mutex_unlock(&g_conn_mutex);

    wait_all_envoys();
    free_envoys();
    ri_free_all(pstCache, nCacheCount);

    /* Close the self-pipe used for prompt notifications */
    if (g_notify_pipe[0] >= 0) {
        close(g_notify_pipe[0]);
        g_notify_pipe[0] = -1;
    }
    if (g_notify_pipe[1] >= 0) {
        close(g_notify_pipe[1]);
        g_notify_pipe[1] = -1;
    }
}
