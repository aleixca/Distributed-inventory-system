/***********************************************
 *
 * @File:    list_handler.c
 * @Purpose: Handles incoming LIST REQUEST frames and implements the outgoing product list request protocol.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "handler_helpers.h"
#include "network.h"
#include "sigil.h"
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
/* Declares g_data_mutex and g_pledge_mutex. */
#include "envoy.h"

/* ── LIST REQUEST (0x11 at ally) ─────────────────────────────────────────── */

/********************
 *
 * @Name: handle_list_request
 * @Def: Handles an incoming 0x11 LIST REQUEST from an allied realm.
 *       Protocol:
 *         1. Verify requester is allied
 *         2. Send 0x12 header: "stock.db&size&md5"
 *         3. Receive 0x31 (requester ready)
 *         4. Send stock.db in 0x13 frames
 *         5. Receive 0x32 (requester MD5 result)
 * @Arg: In:     pstFrame        = received 0x11 frame (DATA = requester realm name)
 *       In:     nClientFd       = connection from requester
 *       In:     pstMaester      = local Maester config
 *       In/Out: ppstProducts    = unused (stock file used directly)
 *       In/Out: pnTotalProducts = unused
 * @Ret: None
 *
 ********************/
void handle_list_request(Frame *pstFrame, int nClientFd, Maester *pstMaester,
                          Product **ppstProducts, int *pnTotalProducts) {
    char  sDataCopy[DATA_SIZE];
    char *psRequesterRealm;
    char  sOrigin[ORIGIN_SIZE];
    char  sHdrData[DATA_SIZE];
    char *psMd5       = NULL;
    int   nFileSize;
    Frame stAck;
    char *psOutput    = NULL;
    int   nLen;

    (void)ppstProducts;
    (void)pnTotalProducts;

    /* Extract requester's realm name from DATA */
    /* DATA in 0x11 is the realm name that is requesting our products. */
    memcpy(sDataCopy, pstFrame->data, DATA_SIZE - 1);
    sDataCopy[DATA_SIZE - 1] = '\0';
    psRequesterRealm = sDataCopy;
    {
        int nDlen = (int)pstFrame->data_length;
        if (nDlen > 0 && nDlen < DATA_SIZE) {
            sDataCopy[nDlen] = '\0';
        }
    }

    // Two independent checks are required: pledge status AND origin IP match.
    // The DATA field is attacker-controlled text — anyone could put "Arryn" in
    // it. The pledge table keyed by IP:Port is the authoritative ground truth
    // for who is actually on the other end of this TCP connection.
    /* Cross-check DATA realm name against the stored IP for the
     * connection's origin — prevents a forged DATA field from bypassing auth. */
    {
        char *psOriginRealm = find_realm_by_origin(pstFrame->origin);
        int  nAllied = (PLEDGE_ALLIED == get_pledge_status(psRequesterRealm));
        int nMatch = (NULL != psOriginRealm && 0 == strcasecmp(psOriginRealm, psRequesterRealm));
        const char *psDestConst;

        if (!nAllied || !nMatch) {
            Frame stUnauth;
            char  sUnauthData[DATA_SIZE];
            short nUdlen;

            if (NULL != psOriginRealm) {
                psDestConst = psOriginRealm;
            } else {
                psDestConst = psRequesterRealm;
            }
            format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);
            nUdlen = (short)snprintf(sUnauthData, DATA_SIZE, "AUTH&%s", pstMaester->realm_name);
            build_frame(&stUnauth, MSG_UNAUTHORIZED, sOrigin, (char *)psDestConst, sUnauthData, nUdlen);
            send_frame(nClientFd, &stUnauth);
            return;
        }
    }

    if (NULL == pstMaester->stock_path) {
        printF("[LIST] No stock file configured.\n");
        return;
    }

    /* LIST sends the current stock database as a binary file. */
    nFileSize = get_file_size(pstMaester->stock_path);
    if (nFileSize < 0) {
        printF("[LIST] Cannot read stock file.\n");
        return;
    }

    /* Receiver will compare this MD5 with the file it reconstructs. */
    psMd5 = compute_file_md5(pstMaester->stock_path);
    if (NULL == psMd5) {
        printF("[LIST] Cannot compute stock MD5.\n");
        return;
    }

    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    // The stock database is sent as a raw binary file transfer (same mechanism
    // as sigil files) rather than encoding each product into DATA fields.
    // This keeps the protocol uniform and avoids a DATA_SIZE (275-byte) limit
    // on inventory size.
    /* Send 0x12 header */
    {
        char  *psStockBasename;
        short  nDlen;

        psStockBasename = strrchr(pstMaester->stock_path, '/');
        if (NULL == psStockBasename) {
            psStockBasename = strrchr(pstMaester->stock_path, '\\');
        }
        if (NULL != psStockBasename) {
            psStockBasename = psStockBasename + 1;
        } else {
            psStockBasename = pstMaester->stock_path;
        }

        nDlen = (short)snprintf(sHdrData, DATA_SIZE, "%s&%d&%s", psStockBasename, nFileSize, psMd5);
        free(psMd5); psMd5 = NULL;

        {
            Frame stHdr;
            build_frame(&stHdr, MSG_LIST_HEADER, sOrigin, psRequesterRealm, sHdrData, nDlen);
            if (send_frame(nClientFd, &stHdr) < 0) {
                return;
            }
        }
    }

    /* Validate ACK FILE checksum */
    /* Wait until requester says it is ready to receive the stock file. */
    if (0 != recv_validated(nClientFd, &stAck, pstMaester)) {
        return;
    }
    if (MSG_ACK_FILE != stAck.type) {
        return;
    }
    if (stAck.data_length >= 2 &&
        stAck.data[0] == 'K' && stAck.data[1] == 'O') {
        return;
    }

    /* Send stock file in 0x13 frames */
    if (0 != send_file_in_frames(nClientFd, pstMaester->stock_path, MSG_LIST_DATA, sOrigin, psRequesterRealm)) {
        return;
    }

    /* Validate ACK MD5 checksum */
    /* Final ACK tells us whether the requester verified the MD5. */
    if (0 != recv_validated(nClientFd, &stAck, pstMaester)) {
        return;
    }

    nLen = asprintf(&psOutput,
        "\n>>> LIST PRODUCTS request from %s. Products delivered.\n",
        psRequesterRealm);
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }
}

/* ── request_list_products ───────────────────────────────────────────────── */

/********************
 *
 * @Name: request_list_products
 * @Def: Requests the product list from an allied realm (0x11 → 0x12 → 0x31
 *       → 0x13 frames → 0x32).  Parses the received binary file into Product
 *       structs, displays them, and stores them in the remote cache.
 * @Arg: In:     psRealm       = allied realm name
 *       In:     pstMaester    = local Maester config
 *       In:     pstProducts   = local product array (unused here)
 *       In:     nTotalProducts = unused
 *       In/Out: ppstCache     = pointer to remote inventory cache array
 *       In/Out: pnCacheCount  = pointer to number of cache entries
 * @Ret: None
 *
 ********************/
void request_list_products(char *psRealm, Maester *pstMaester, Product *pstProducts, int nTotalProducts, RemoteInventory **ppstCache, int *pnCacheCount) {
    char  sOrigin[ORIGIN_SIZE];
    char  sAIp[IP_SIZE];
    int   nAPort;
    Frame stReq;
    Frame stHdr;
    int   nFd;
    char *psOutput = NULL;
    int   nLen;
    char  sDataCopy[DATA_SIZE];
    char *psHdrFname = NULL;
    char *psHdrSizeStr = NULL;
    char *psHdrMd5 = NULL;
    int   nExpectedSize;
    char  sSavePath[512];
    char *psReceivedMd5 = NULL;

    (void)pstProducts;
    (void)nTotalProducts;

    // Guard is on the requester side too: even if the remote accepts the
    // connection, we will not send 0x11 unless we consider ourselves allied.
    // This prevents accidentally leaking our direct IP:Port to a realm whose
    // pledge we have not yet accepted (the 0x11 DATA contains our realm name
    // and the ORIGIN field contains our listen address).
    /* Product lists may only be requested from allied realms. */
    if (PLEDGE_ALLIED != get_pledge_status(psRealm)) {
        {
            char *psOut2 = NULL;
            int   nLen2  = asprintf(&psOut2,
                "The gates of commerce with %s remain closed; "
                "no alliance binds you.\n", psRealm);
            if (-1 != nLen2) {
                printF(psOut2);
                free(psOut2);
            }
        }
        return;
    }

    /* Allied pledge stores the direct IP/port for this request. */
    if (0 != get_pledge_ip_port(psRealm, sAIp, &nAPort)) {
        nLen = asprintf(&psOutput, "Cannot find address for %s.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return;
    }

    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    /* Build 0x11 request: DATA = our realm name */
    {
        short nDlen = (short)strlen(pstMaester->realm_name);
        build_frame(&stReq, MSG_LIST_REQUEST, sOrigin, psRealm,
                    pstMaester->realm_name, nDlen);
    }

    nFd = connect_to_realm(sAIp, nAPort);
    if (nFd < 0) {
        nLen = asprintf(&psOutput, "Could not connect to %s.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return;
    }

    /* Send 0x11 to begin the LIST PRODUCTS protocol. */
    if (send_frame(nFd, &stReq) < 0) {
        close(nFd);
        return;
    }

    /* Receive 0x12 header — validate checksum */
    if (0 != recv_validated(nFd, &stHdr, pstMaester) ||
        MSG_LIST_HEADER != stHdr.type) {
        printF("No valid list header received.\n");
        close(nFd);
        return;
    }

    memcpy(sDataCopy, stHdr.data, DATA_SIZE - 1);
    sDataCopy[DATA_SIZE - 1] = '\0';
    parse_data_fields(sDataCopy, (int)stHdr.data_length, &psHdrFname, &psHdrSizeStr, &psHdrMd5, NULL);

    if (NULL == psHdrFname || NULL == psHdrSizeStr || NULL == psHdrMd5) {
        printF("Malformed list header.\n");
        close(nFd);
        return;
    }

    /* The header tells exactly how many bytes of stock file to receive. */
    nExpectedSize = atoi(psHdrSizeStr);

    /* Prepare path to save received binary */
    if (NULL != pstMaester->user_dir) {
        if (0 != ensure_dir(pstMaester->user_dir)) {
            printF("[LIST] Cannot create user directory.\n");
            close(nFd);
            return;
        }
        snprintf(sSavePath, sizeof(sSavePath), "%s/%s_stock.bin", pstMaester->user_dir, psRealm);
    } else {
        snprintf(sSavePath, sizeof(sSavePath), "%s_stock.bin", psRealm);
    }

    /* Send 0x31 OK — ready to receive */
    send_ack_file(nFd, sOrigin, psRealm, "OK", pstMaester->realm_name);

    /* Receive stock file in 0x13 frames — handle checksum error with NACK */
    {
        int nRc = recv_file_in_frames(nFd, sSavePath, nExpectedSize, MSG_LIST_DATA);
        if (RECV_FRAMES_CKSUM == nRc) {
            send_nack_on_fd(nFd, pstMaester);
            send_ack_md5(nFd, sOrigin, psRealm, "CHECK_KO", pstMaester->realm_name);
            unlink(sSavePath);
            close(nFd);
            return;
        }
        if (RECV_FRAMES_OK != nRc) {
            printF("Error receiving product list.\n");
            send_ack_md5(nFd, sOrigin, psRealm, "CHECK_KO", pstMaester->realm_name);
            unlink(sSavePath);
            close(nFd);
            return;
        }
    }

    /* Verify MD5 */
    psReceivedMd5 = compute_file_md5(sSavePath);
    if (NULL == psReceivedMd5 ||
        0 != strncmp(psReceivedMd5, psHdrMd5, 32)) {
        send_ack_md5(nFd, sOrigin, psRealm, "CHECK_KO", pstMaester->realm_name);
        unlink(sSavePath);
        free(psReceivedMd5);
        close(nFd);
        return;
    }
    free(psReceivedMd5);

    send_ack_md5(nFd, sOrigin, psRealm, "CHECK_OK", pstMaester->realm_name);
    close(nFd);

    /* Parse received binary into Product[] (same layout as local stock.db) */
    {
        int nProdCount  = 0;
        Product *pstAllyProds = NULL;
        int nSaveFd = open(sSavePath, O_RDONLY);
        Product stTmp;
        int j;

        if (-1 != nSaveFd) {
            while (read(nSaveFd, &stTmp, sizeof(Product)) == (ssize_t)sizeof(Product)) {
                Product *pstNp = realloc(pstAllyProds, (size_t)(nProdCount + 1) * sizeof(Product));
                if (NULL == pstNp) {
                    break;
                }
                pstAllyProds = pstNp;
                pstAllyProds[nProdCount++] = stTmp;
            }
            close(nSaveFd);
        }

        /* Display */
        nLen = asprintf(&psOutput, "Listing products from %s:\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }

        for (j = 0; j < nProdCount; j++) {
            nLen = asprintf(&psOutput, "%d. %s (%d units)\n", j + 1, pstAllyProds[j].name, pstAllyProds[j].amount);
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
        }

        // The remote cache is shared between the main thread (START TRADE reads
        // it to populate the order) and envoy children (LIST runs in a child).
        // g_data_mutex serialises all writes so a concurrent trade cannot read
        // a half-updated cache entry.
        /* Update cache — lock while modifying shared data */
        if (nProdCount > 0) {
            pthread_mutex_lock(&g_data_mutex);
            *ppstCache = ri_set_products(*ppstCache, pnCacheCount, psRealm, pstAllyProds, nProdCount);
            pthread_mutex_unlock(&g_data_mutex);
        }
        free(pstAllyProds);
    }
    unlink(sSavePath);
}
