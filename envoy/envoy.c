/***********************************************
 *
 * @File:    envoy.c
 * @Purpose: Manages the envoy pool of forked child processes that handle outgoing missions (pledge, list, trade, respond).
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "envoy.h"
#include "message_handler.h"
#include "io.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

/* ── Global state ─────────────────────────────────────────────────────────── */

static Envoy *g_envoys = NULL;
static int g_count = 0;
/* Parent-owned config. */
static Maester *g_maester = NULL;
/* Parent inventory pointer. */
static Product **g_products = NULL;
/* Parent inventory count. */
static int *g_total_products = NULL;
/* Parent remote cache. */
static RemoteInventory **g_cache = NULL;
/* Parent remote cache size. */
static int *g_cache_count = NULL;

/* g_pledge_mutex is defined in pledge.c (recursive).
 * g_data_mutex protects products[] and the remote inventory cache. */
pthread_mutex_t g_data_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── pthread_atfork: reinit mutexes in child so they start unlocked ───────── */

static void child_reinit_mutexes(void) {
    /* After fork(), only the calling thread exists in the child. If another
     * thread held a mutex at fork time, the child could inherit it locked
     * forever. Reinitialising avoids that deadlock. */
    pthread_mutex_init(&g_data_mutex, NULL);
    pthread_mutex_init(&g_pledge_mutex, NULL);
}

/* ── Child worker functions ───────────────────────────────────────────────── */
/* Fork+pipe pattern used by all child workers:
 *   1. Parent calls pipe(), then fork().
 *   2. Child closes the read end; parent closes the write end.
 *   3. Child performs the blocking network mission (connect, send, recv).
 *   4. Child writes exactly one PipeMsgHeader (+ optional Product payload)
 *      to the write end, then calls exit(0).
 *   5. Parent's select() loop detects the read end becoming readable and
 *      calls apply_envoy_result() to consume the result and update state.
 * This keeps the terminal's select() loop non-blocking: every network
 * operation that could stall is isolated inside a child process. */

static void child_pledge(int nResultFd, char *psRealm, char *psSigil, Maester *pstMaester) {
    PipeMsgHeader stMsg;
    int nOk = send_pledge(psRealm, psSigil, pstMaester);

    /* Children do blocking network work, then report only the result by pipe. */
    memset(&stMsg, 0, sizeof(stMsg));
    if (0 == nOk) {
        stMsg.type = PIPE_PLEDGE_OK;
    } else {
        stMsg.type = PIPE_PLEDGE_FAIL;
    }
    strncpy(stMsg.realm, psRealm, 49);
    stMsg.realm[49] = '\0';
    stMsg.count = 0;
    /* The parent reads this header in apply_envoy_result(). */
    write_all(nResultFd, &stMsg, sizeof(stMsg));
    close(nResultFd);
    exit(0);
}

static void child_list(int nResultFd, char *psRealm, Maester *pstMaester, Product *pstProducts, int nTotalProducts, RemoteInventory **ppstCache, int *pnCacheCount) {
    PipeMsgHeader stHdr;
    int nCnt = 0;
    Product *pstP;

    /* The child performs the LIST protocol without blocking the terminal. */
    request_list_products(psRealm, pstMaester, pstProducts, nTotalProducts,ppstCache, pnCacheCount);

    /* Then it sends the received remote inventory snapshot to the parent. */
    pstP = ri_get_products(*ppstCache, *pnCacheCount, psRealm, &nCnt);
    memset(&stHdr, 0, sizeof(stHdr));
    if (NULL != pstP) {
        stHdr.type = PIPE_LIST_OK;
    } else {
        stHdr.type = PIPE_LIST_FAIL;
    }
    strncpy(stHdr.realm, psRealm, 49);
    stHdr.realm[49] = '\0';
    stHdr.count = nCnt;
    write_all(nResultFd, &stHdr, sizeof(stHdr));
    if (NULL != pstP && nCnt > 0) {
        write_all(nResultFd, pstP, (size_t)nCnt * sizeof(Product));
    }
    close(nResultFd);
    exit(0);
}

static void child_trade(int nResultFd, char *psRealm, Maester *pstMaester, Product **ppstProducts, int *pnTotalProducts, TradeItem *pstItems, int nCount, RemoteInventory **ppstCache, int *pnCacheCount, int nIdx) {
    PipeMsgHeader stHdr;
    int nOk;

    nOk = send_trade_request(psRealm, pstMaester, ppstProducts, pnTotalProducts, pstItems, nCount, ppstCache, pnCacheCount, nIdx);
    memset(&stHdr, 0, sizeof(stHdr));
    strncpy(stHdr.realm, psRealm, 49);
    stHdr.realm[49] = '\0';

    if (0 == nOk) {
        /* Build a gain Product[] — what we received from the ally.
         * The parent will apply this as a delta so concurrent incoming
         * trades are not overwritten by a full-array replacement. */
        Product *pstGain = malloc((size_t)nCount * sizeof(Product));
        int i;

        if (NULL != pstGain) {
            for (i = 0; i < nCount; i++) {
                float fW = 0.0f;
                int j;
                int nAllyCount = 0;
                /* Weight is copied from the cached inventory of the ally. */
                Product *pstAlly = ri_get_products(*ppstCache, *pnCacheCount, psRealm, &nAllyCount);
                for (j = 0; NULL != pstAlly && j < nAllyCount; j++) {
                    if (0 == strcasecmp(pstAlly[j].name, pstItems[i].name)) {
                        fW = pstAlly[j].weight;
                        break;
                    }
                }
                memset(&pstGain[i], 0, sizeof(Product));
                strncpy(pstGain[i].name, pstItems[i].name, 99);
                pstGain[i].name[99] = '\0';
                pstGain[i].amount = pstItems[i].amount;
                pstGain[i].weight = fW;
            }
            stHdr.type  = PIPE_TRADE_OK;
            stHdr.count = nCount;
            write_all(nResultFd, &stHdr, sizeof(stHdr));
            write_all(nResultFd, pstGain, (size_t)nCount * sizeof(Product));
            free(pstGain);
        } else {
            stHdr.type  = PIPE_TRADE_FAIL;
            stHdr.count = 0;
            write_all(nResultFd, &stHdr, sizeof(stHdr));
        }
    } else {
        stHdr.type  = PIPE_TRADE_FAIL;
        stHdr.count = 0;
        write_all(nResultFd, &stHdr, sizeof(stHdr));
    }
    free_trade_items(pstItems, nCount);
    close(nResultFd);
    exit(0);
}

static void child_respond(int nResultFd, char *psRealm, char *psResponse, Maester *pstMaester) {
    PipeMsgHeader stHdr;
    int nNewStatus = send_pledge_response(psRealm, psResponse, pstMaester);
    /* Response missions only need to return the resulting pledge status. */
    memset(&stHdr, 0, sizeof(stHdr));
    stHdr.type  = PIPE_RESPOND_DONE;
    stHdr.count = nNewStatus;
    strncpy(stHdr.realm, psRealm, 49);
    stHdr.realm[49] = '\0';
    write_all(nResultFd, &stHdr, sizeof(stHdr));
    close(nResultFd);
    exit(0);
}

/* ── apply_envoy_result ───────────────────────────────────────────────────── */

/********************
 *
 * @Name: apply_envoy_result
 * @Def: Called from terminal's select() loop when an envoy's pipe is readable.
 *       Reads the PipeMsgHeader (+ optional Product payload), applies the
 *       state mutation to the parent's shared data, reaps the child, and
 *       marks the slot FREE.
 * @Arg: In: nIdx = envoy slot index
 * @Ret: None
 *
 ********************/
void apply_envoy_result(int nIdx) {
    Envoy *pstE = &g_envoys[nIdx];
    PipeMsgHeader stHdr;
    int nKeepReserved = 0;
    int nReadOk = 1;

    /* First read the fixed-size result header written by the child. */
    if (0 != read_all(pstE->pipe_read_fd, &stHdr, sizeof(stHdr))) {
        nReadOk = 0;
    }

    if (nReadOk) {
        if (PIPE_LIST_OK == stHdr.type && stHdr.count > 0) {
            /* LIST returns a full Product[] snapshot for one remote realm. */
            Product *pstProds = malloc((size_t)stHdr.count * sizeof(Product));
            if (NULL != pstProds) {
                if (0 == read_all(pstE->pipe_read_fd, pstProds, (size_t)stHdr.count * sizeof(Product))) {
                    pthread_mutex_lock(&g_data_mutex);
                    *g_cache = ri_set_products(*g_cache, g_cache_count, stHdr.realm, pstProds, stHdr.count);
                    pthread_mutex_unlock(&g_data_mutex);
                }
                free(pstProds);
            }
        } else if (PIPE_TRADE_OK == stHdr.type && stHdr.count > 0) {
            /* TRADE returns the products gained, parent applies them to stock. */
            Product *pstProds = malloc((size_t)stHdr.count * sizeof(Product));
            if (NULL != pstProds) {
                if (0 == read_all(pstE->pipe_read_fd, pstProds, (size_t)stHdr.count * sizeof(Product))) {
                    /* Apply the gain as a delta so concurrent incoming-trade
                     * thread updates to *g_products are not overwritten. */
                    int k;
                    pthread_mutex_lock(&g_data_mutex);
                    for (k = 0; k < stHdr.count; k++) {
                        add_or_update_product(g_products, g_total_products, pstProds[k].name, pstProds[k].amount, pstProds[k].weight);
                    }
                    if (NULL != g_maester->stock_path) {
                        write_stock_file(g_maester->stock_path, *g_products, *g_total_products);
                    }
                    pthread_mutex_unlock(&g_data_mutex);
                }
                free(pstProds);
            }
        } else if (PIPE_PLEDGE_OK == stHdr.type) {
            /* Keep envoy reserved while the remote realm has not answered yet. */
            add_outgoing_pledge(stHdr.realm);
            nKeepReserved = 1;
        } else if (PIPE_RESPOND_DONE == stHdr.type) {
            update_pledge_status(stHdr.realm, stHdr.count);
        } else if (PIPE_PLEDGE_FAIL == stHdr.type) {
            /* Sent when send_pledge() returned non-zero: connection refused,
             * sigil file not found, MD5 mismatch, or realm not reachable.
             * No pledge table entry is created so the slot can be retried. */
            printF(">>> Envoy: pledge mission failed (connection error or bad sigil).\n");
        } else if (PIPE_LIST_FAIL == stHdr.type) {
            /* Sent when request_list_products() got no valid LIST_DATA back.
             * The remote cache for that realm is left unchanged. */
            printF(">>> Envoy: list products mission failed.\n");
        } else if (PIPE_TRADE_FAIL == stHdr.type) {
            /* Sent when send_trade_request() returned non-zero or the ally
             * responded with ORDER_RESP reject.  Our stock is not modified. */
            printF(">>> Envoy: trade mission failed or order was rejected.\n");
        }
    }

    /* Reap child process and close its parent-side pipe. */
    waitpid(pstE->pid, NULL, 0);
    close(pstE->pipe_read_fd);
    pstE->pid = -1;
    pstE->pipe_read_fd = -1;
    if (!nKeepReserved) {
        pstE->mission  = ENVOY_FREE;
        pstE->realm[0] = '\0';
    }
    notify_prompt();
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/********************
 *
 * @Name: init_envoys
 * @Def: Allocates and initialises N envoy slots; stores shared-state pointers.
 *       Registers pthread_atfork handler so forked children start with unlocked
 *       mutexes regardless of connection-thread lock state at fork time.
 * @Arg: In:     nCount          = number of envoys from config
 *       In:     pstMaester      = local Maester config
 *       In/Out: ppstProducts    = pointer to terminal's products pointer
 *       In/Out: pnTotalProducts = pointer to terminal's product count
 *       In/Out: ppstCache       = pointer to terminal's remote inventory cache
 *       In/Out: pnCacheCount    = pointer to terminal's cache count
 * @Ret: None
 *
 ********************/
void init_envoys(int nCount, Maester *pstMaester, Product **ppstProducts, int *pnTotalProducts, RemoteInventory **ppstCache, int *pnCacheCount) {
    int i;

    /* Store shared pointers once so dispatch functions can access them later. */
    g_count = nCount;
    g_maester = pstMaester;
    g_products = ppstProducts;
    g_total_products = pnTotalProducts;
    g_cache = ppstCache;
    g_cache_count = pnCacheCount;

    g_envoys = calloc((size_t)nCount, sizeof(Envoy));
    if (NULL == g_envoys) {
        return;
    }

    for (i = 0; i < nCount; i++) {
        g_envoys[i].id = i + 1;
        g_envoys[i].mission = ENVOY_FREE;
        g_envoys[i].realm[0] = '\0';
        g_envoys[i].pid = -1;
        g_envoys[i].pipe_read_fd = -1;
    }

    /* Only the child handler is needed: parent keeps its existing mutexes. */
    pthread_atfork(NULL, NULL, child_reinit_mutexes);
}

/********************
 *
 * @Name: find_free_envoy
 * @Def: Scans the envoy array for the first FREE slot.
 * @Arg: None
 * @Ret: Index of a free envoy (0-based), or -1 if all are occupied.
 *
 ********************/
int find_free_envoy() {
    int i;

    for (i = 0; i < g_count; i++) {
        if (ENVOY_FREE == g_envoys[i].mission){
             return i;
        }
    }
    return -1;
}

/********************
 *
 * @Name: reserve_envoy
 * @Def: Marks a slot as busy (mission + realm set) without forking.
 * @Arg: In: nIdx     = slot index
 *       In: eMission = mission type to assign
 *       In: psRealm  = target realm name
 * @Ret: None
 *
 ********************/
void reserve_envoy(int nIdx, EnvoyMission eMission, char *psRealm) {
    g_envoys[nIdx].mission = eMission;
    strncpy(g_envoys[nIdx].realm, psRealm, 49);
    g_envoys[nIdx].realm[49] = '\0';
}

/********************
 *
 * @Name: release_envoy
 * @Def: Releases a reserved-but-not-started slot back to FREE.
 * @Arg: In: nIdx = slot index
 * @Ret: None
 *
 ********************/
void release_envoy(int nIdx) {
    g_envoys[nIdx].mission = ENVOY_FREE;
    g_envoys[nIdx].realm[0] = '\0';
    g_envoys[nIdx].pid = -1;
    g_envoys[nIdx].pipe_read_fd = -1;
}

/* ── Mission dispatch ─────────────────────────────────────────────────────── */

/********************
 *
 * @Name: dispatch_pledge
 * @Def: Forks a child process for a PLEDGE mission.
 * @Arg: In: nIdx     = envoy slot index
 *       In: psRealm  = target realm name
 *       In: psSigil  = sigil file path
 * @Ret: None
 *
 ********************/
void dispatch_pledge(int nIdx, char *psRealm, char *psSigil) {
    int nPipefd[2];
    pid_t nPid;

    g_envoys[nIdx].mission = ENVOY_PLEDGE;
    strncpy(g_envoys[nIdx].realm, psRealm, 49);
    g_envoys[nIdx].realm[49] = '\0';

    /* The pipe is the child-to-parent result channel. */
    if (0 != pipe(nPipefd)) {
        release_envoy(nIdx);
        return;
    }

    nPid = fork();
    if (nPid < 0) {
        close(nPipefd[0]);
        close(nPipefd[1]);
        release_envoy(nIdx);
        return;
    }
    if (0 == nPid) {
        /* Child keeps write end only. */
        close(nPipefd[0]);
        child_pledge(nPipefd[1], psRealm, psSigil, g_maester);
    }
    /* parent */
    close(nPipefd[1]);
    g_envoys[nIdx].pid = nPid;
    g_envoys[nIdx].pipe_read_fd = nPipefd[0];
}

/********************
 *
 * @Name: dispatch_list_products
 * @Def: Forks a child process for a LIST PRODUCTS mission.
 * @Arg: In: nIdx    = envoy slot index
 *       In: psRealm = target realm name
 * @Ret: None
 *
 ********************/
void dispatch_list_products(int nIdx, char *psRealm) {
    int   nPipefd[2];
    pid_t nPid;

    g_envoys[nIdx].mission = ENVOY_LIST;
    strncpy(g_envoys[nIdx].realm, psRealm, 49);
    g_envoys[nIdx].realm[49] = '\0';

    if (0 != pipe(nPipefd)) {
        release_envoy(nIdx);
        return;
    }

    nPid = fork();
    if (nPid < 0) {
        close(nPipefd[0]);
        close(nPipefd[1]);
        release_envoy(nIdx);
        return;
    }
    if (0 == nPid) {
        /* Child receives fork-time snapshots/pointers. Any lasting update must
         * be sent back through the pipe because forked memory is private. */
        RemoteInventory *pstChildCache;
        int nChildCacheCount;
        Product *pstChildProductsArg;
        int nChildTotalArg;

        if (NULL != g_cache) {
            pstChildCache = *g_cache;
        } else {
            pstChildCache = NULL;
        }
        if (NULL != g_cache_count) {
            nChildCacheCount = *g_cache_count;
        } else {
            nChildCacheCount = 0;
        }
        if (NULL != g_products) {
            pstChildProductsArg = *g_products;
        } else {
            pstChildProductsArg = NULL;
        }
        if (NULL != g_total_products) {
            nChildTotalArg = *g_total_products;
        } else {
            nChildTotalArg = 0;
        }
        close(nPipefd[0]);
        child_list(nPipefd[1], psRealm, g_maester, pstChildProductsArg, nChildTotalArg, &pstChildCache, &nChildCacheCount);
    }
    /* parent */
    close(nPipefd[1]);
    g_envoys[nIdx].pid = nPid;
    g_envoys[nIdx].pipe_read_fd = nPipefd[0];
}

/********************
 *
 * @Name: dispatch_trade
 * @Def: Forks a child process for a TRADE mission.
 * @Arg: In: nIdx      = envoy slot index (already reserved)
 *       In: psRealm   = target realm name
 *       In: pstItems  = trade items (ownership transferred to child)
 *       In: nCount    = number of trade items
 * @Ret: None
 *
 ********************/
void dispatch_trade(int nIdx, char *psRealm, TradeItem *pstItems, int nCount) {
    int nPipefd[2];
    pid_t nPid;

    if (0 != pipe(nPipefd)) {
        free_trade_items(pstItems, nCount);
        release_envoy(nIdx);
        return;
    }

    nPid = fork();
    if (nPid < 0) {
        close(nPipefd[0]);
        close(nPipefd[1]);
        free_trade_items(pstItems, nCount);
        release_envoy(nIdx);
        return;
    }
    if (0 == nPid) {
        /* Child owns its forked copy of pstItems and frees it before exit. */
        Product *pstChildProducts;
        int nChildTotal;
        RemoteInventory *pstChildCache;
        int nChildCacheCount;

        if (NULL != g_products) {
            pstChildProducts = *g_products;
        } else {
            pstChildProducts = NULL;
        }
        if (NULL != g_total_products) {
            nChildTotal = *g_total_products;
        } else {
            nChildTotal = 0;
        }
        if (NULL != g_cache) {
            pstChildCache = *g_cache;
        } else {
            pstChildCache = NULL;
        }
        if (NULL != g_cache_count) {
            nChildCacheCount = *g_cache_count;
        } else {
            nChildCacheCount = 0;
        }
        close(nPipefd[0]);
        child_trade(nPipefd[1], psRealm, g_maester, &pstChildProducts, &nChildTotal, pstItems, nCount, &pstChildCache, &nChildCacheCount, nIdx);
    }
    /* parent: free our copy of items (child has its own fork-copy) */
    free_trade_items(pstItems, nCount);
    close(nPipefd[1]);
    g_envoys[nIdx].pid = nPid;
    g_envoys[nIdx].pipe_read_fd = nPipefd[0];
    /* Realm was already set by reserve_envoy. */
    (void)psRealm;
}

/********************
 *
 * @Name: dispatch_pledge_respond
 * @Def: Forks a child for a PLEDGE RESPOND mission (ACCEPT or REJECT).
 * @Arg: In: nIdx       = envoy slot index
 *       In: psRealm    = realm to respond to
 *       In: psResponse = "ACCEPT" or "REJECT"
 * @Ret: None
 *
 ********************/
void dispatch_pledge_respond(int nIdx, char *psRealm, char *psResponse) {
    int   nPipefd[2];
    pid_t nPid;

    g_envoys[nIdx].mission = ENVOY_RESPOND;
    strncpy(g_envoys[nIdx].realm, psRealm, 49);
    g_envoys[nIdx].realm[49] = '\0';

    if (0 != pipe(nPipefd)) {
        release_envoy(nIdx);
        return;
    }

    nPid = fork();
    if (nPid < 0) {
        close(nPipefd[0]);
        close(nPipefd[1]);
        release_envoy(nIdx);
        return;
    }
    if (0 == nPid) {
        /* Copy response text into child stack before running the mission. */
        char sRespCopy[8];
        strncpy(sRespCopy, psResponse, 7);
        sRespCopy[7] = '\0';
        close(nPipefd[0]);
        child_respond(nPipefd[1], psRealm, sRespCopy, g_maester);
    }
    close(nPipefd[1]);
    g_envoys[nIdx].pid = nPid;
    g_envoys[nIdx].pipe_read_fd = nPipefd[0];
}

/********************
 *
 * @Name: check_pledge_envoys
 * @Def: Scans PLEDGE-state envoys whose child exited; releases slots no longer
 *       in OUTGOING_PENDING (accepted, rejected, or timed out).
 * @Arg: None
 * @Ret: None
 *
 ********************/
void check_pledge_envoys(void) {
    int i;

    for (i = 0; i < g_count; i++) {
        if (ENVOY_PLEDGE != g_envoys[i].mission) {
            continue;
        }
        /* A positive pid means the child is still running. */
        if (g_envoys[i].pid > 0) {
            continue;
        }
        if (PLEDGE_OUTGOING_PENDING !=
            get_pledge_status(g_envoys[i].realm)) {
            g_envoys[i].mission  = ENVOY_FREE;
            g_envoys[i].realm[0] = '\0';
            notify_prompt();
        }
    }
}

/* ── Status and accessors ─────────────────────────────────────────────────── */

/********************
 *
 * @Name: get_envoy_pipe_fd
 * @Def: Returns the pipe read fd for envoy nIdx, or -1 if free/invalid.
 * @Arg: In: nIdx = envoy slot index
 * @Ret: Pipe read fd, or -1
 *
 ********************/
int get_envoy_pipe_fd(int nIdx) {
    if (nIdx < 0 || nIdx >= g_count) {
        return -1;
    }
    return g_envoys[nIdx].pipe_read_fd;
}

/********************
 *
 * @Name: show_envoy_status
 * @Def: Prints the current status of every envoy to stdout.
 * @Arg: None
 * @Ret: None
 *
 ********************/
void show_envoy_status() {
    int i;

    for (i = 0; i < g_count; i++) {
        char *psOutput = NULL;
        int nLen;
        char *psMname;

        if (ENVOY_FREE == g_envoys[i].mission) {
            nLen = asprintf(&psOutput, "- Envoy %d: FREE\n", g_envoys[i].id);
        } else {
            switch (g_envoys[i].mission) {
                case ENVOY_PLEDGE:  
                psMname = "PLEDGE";         
                break;
                case ENVOY_LIST:    psMname = "LIST PRODUCTS";  break;
                case ENVOY_TRADE:   psMname = "TRADE";          break;
                case ENVOY_RESPOND: psMname = "PLEDGE RESPOND"; break;
                default:            psMname = "MISSION";        break;
            }
            nLen = asprintf(&psOutput,
                            "- Envoy %d: ON MISSION (%s to %s)\n",
                            g_envoys[i].id, psMname, g_envoys[i].realm);
        }
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
    }
}

/********************
 *
 * @Name: wait_all_envoys
 * @Def: Drains all active envoy pipes and waits for children (called on EXIT).
 * @Arg: None
 * @Ret: None
 *
 ********************/
void wait_all_envoys(void) {
    int i;

    for (i = 0; i < g_count; i++) {
        if (g_envoys[i].pid > 0) {
            apply_envoy_result(i);
        } else if (ENVOY_FREE != g_envoys[i].mission) {
            release_envoy(i);
        }
    }
}

/********************
 *
 * @Name: free_envoys
 * @Def: Closes any remaining pipe fds, destroys g_data_mutex, frees the array.
 * @Arg: None
 * @Ret: None
 *
 ********************/
void free_envoys(void) {
    int i;

    if (NULL == g_envoys) {
        return;
    }
    for (i = 0; i < g_count; i++) {
        if (g_envoys[i].pipe_read_fd >= 0) {
            close(g_envoys[i].pipe_read_fd);
            g_envoys[i].pipe_read_fd = -1;
        }
    }
    pthread_mutex_destroy(&g_data_mutex);
    free(g_envoys);
    g_envoys = NULL;
    g_count  = 0;
}
