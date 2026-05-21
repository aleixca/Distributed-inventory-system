/***********************************************
 *
 * @File:    order_handler.c
 * @Purpose: Handles incoming trade ORDER frames and implements the outgoing trade request protocol.
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

/* ── ORDER HEADER (0x14 at ally) ─────────────────────────────────────────── */

/* Aggregated order line: sum of all quantities for one product name. */
struct OrderLine { 
    char name[100]; 
    int amount; 
};

/* Process one "name&qty" text line into the aggregated array.
 * Returns a static reject string on error, NULL on success. */
static const char *aggregate_order_line(const char *psLine, struct OrderLine *pstAgg, int *pnAggCount) {
    char  sBuf[256];
    char *psAmp;
    char *psEp;
    long  nQty;
    int   k;

    strncpy(sBuf, psLine, sizeof(sBuf) - 1);
    sBuf[sizeof(sBuf) - 1] = '\0';

    /* Order lines are "PRODUCT&AMOUNT"; missing separator means bad product. */
    psAmp = strchr(sBuf, '&');
    if (NULL == psAmp) {
        return "REJECT&UNKNOWN_PRODUCT";
    }
    *psAmp = '\0';

    nQty = strtol(psAmp + 1, &psEp, 10);
    if (psEp == psAmp + 1 || *psEp != '\0' || nQty <= 0 || nQty > INT_MAX)
        return "REJECT&UNKNOWN_PRODUCT";

    // Aggregation must happen before stock validation, not after.
    // If the same product appears twice (e.g. "Iron&3\nIron&4"), checking
    // each line individually would pass (3 <= stock, 4 <= stock) even though
    // the combined demand of 7 would exceed available stock. Summing first
    // means the single validation pass sees the true total required.
    /* Duplicate product lines are summed before stock validation. */
    for (k = 0; k < *pnAggCount; k++) {
        if (0 == strcasecmp(pstAgg[k].name, sBuf)) {
            pstAgg[k].amount += (int)nQty;
            return NULL;
        }
    }
    if (*pnAggCount >= 256) {
        return "REJECT&UNKNOWN_PRODUCT";
    }
    strncpy(pstAgg[*pnAggCount].name, sBuf, 99);
    pstAgg[*pnAggCount].name[99] = '\0';
    pstAgg[*pnAggCount].amount   = (int)nQty;
    (*pnAggCount)++;
    return NULL;
}

/********************
 *
 * @Name: handle_order_header
 * @Def: Handles an incoming 0x14 ORDER HEADER (trade request) from an ally.
 *       Protocol:
 *         1. Parse DATA: "FileName&FileSize&MD5SUM"
 *         2. Send 0x31 OK
 *         3. Receive order file in 0x15 frames → save to temp file
 *         4. Verify MD5; send 0x32 CHECK_OK or CHECK_KO
 *         5. Parse order file; check/apply stock
 *         6. Send 0x16 "OK" or "REJECT&OUT_OF_STOCK"
 * @Arg: In:     pstFrame      = received 0x14 frame
 *       In:     nClientFd     = connection from buyer
 *       In:     pstMaester    = local Maester config
 *       In/Out: ppstProducts  = local product array (modified on success)
 *       In/Out: pnTotal       = number of local products
 * @Ret: None
 *
 ********************/
void handle_order_header(Frame *pstFrame, int nClientFd, Maester *pstMaester, Product **ppstProducts, int *pnTotal) {
    char sDataCopy[DATA_SIZE];
    char *psFname = NULL;
    char *psSizeStr = NULL;
    char *psExpectedMd5 = NULL;
    int nFileSize;
    char sOrigin[ORIGIN_SIZE];
    char *psOutput = NULL;
    int nLen;
    char sRecvPath[256];
    char *psReceivedMd5 = NULL;
    const char *psRejectReason = NULL;
    char sSenderRealm[REALM_NAME_SIZE];

    /* Verify the sender is an allied realm; record canonical realm name. */
    {
        char *psS = find_realm_by_origin(pstFrame->origin);
        if (NULL == psS || PLEDGE_ALLIED != get_pledge_status(psS)) {
            Frame stUnauth;
            char sUnauthData[DATA_SIZE];
            short nUdlen;
            const char *psUnauthDest;

            format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);
            nUdlen = (short)snprintf(sUnauthData, DATA_SIZE, "AUTH&%s", pstMaester->realm_name);
            if (NULL != psS) {
                psUnauthDest = psS;
            } else {
                psUnauthDest = "";
            }
            build_frame(&stUnauth, MSG_UNAUTHORIZED, sOrigin, (char *)psUnauthDest, sUnauthData, nUdlen);
            send_frame(nClientFd, &stUnauth);
            printF("[ORDER] Rejected: sender is not an allied realm.\n");
            return;
        }
        strncpy(sSenderRealm, psS, REALM_NAME_SIZE - 1);
        sSenderRealm[REALM_NAME_SIZE - 1] = '\0';
    }

    /* 0x14 DATA announces the file that will arrive in 0x15 chunks. */
    memcpy(sDataCopy, pstFrame->data, DATA_SIZE - 1);
    sDataCopy[DATA_SIZE - 1] = '\0';
    parse_data_fields(sDataCopy, (int)pstFrame->data_length, &psFname, &psSizeStr, &psExpectedMd5, NULL);

    if (NULL == psFname || NULL == psSizeStr || NULL == psExpectedMd5) {
        printF("[ORDER] Malformed 0x14 header — ignoring.\n");
        return;
    }

    /* The announced size decides how many bytes recv_file_in_frames expects. */
    nFileSize = atoi(psSizeStr);
    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    /* Send 0x31 OK — ready to receive order file */
    send_ack_file(nClientFd, sOrigin, sSenderRealm, "OK", pstMaester->realm_name);

    /* Prepare receive path — thread ID avoids collisions with concurrent trades */
    if (NULL != pstMaester->user_dir) {
        if (0 != ensure_dir(pstMaester->user_dir)) {
            send_ack_md5(nClientFd, sOrigin, sSenderRealm, "CHECK_KO", pstMaester->realm_name);
            return;
        }
        snprintf(sRecvPath, sizeof(sRecvPath), "%s/recv_order_%lu.txt", pstMaester->user_dir, (unsigned long)pthread_self());
    } else {
        snprintf(sRecvPath, sizeof(sRecvPath), "recv_order_%lu.txt", (unsigned long)pthread_self());
    }

    /* Receive order file in 0x15 frames */
    if (nFileSize <= 0) {
        send_ack_md5(nClientFd, sOrigin, sSenderRealm, "CHECK_KO", pstMaester->realm_name);
        return;
    }
    {
        int nRc = recv_file_in_frames(nClientFd, sRecvPath, nFileSize, MSG_ORDER_DATA);
        if (RECV_FRAMES_CKSUM == nRc) {
            send_nack_on_fd(nClientFd, pstMaester);
            send_ack_md5(nClientFd, sOrigin, sSenderRealm, "CHECK_KO", pstMaester->realm_name);
            return;
        }
        if (RECV_FRAMES_OK != nRc) {
            send_ack_md5(nClientFd, sOrigin, sSenderRealm, "CHECK_KO", pstMaester->realm_name);
            return;
        }
    }

    /* Verify MD5 */
    /* Verify that the reconstructed order file matches the announced MD5. */
    psReceivedMd5 = compute_file_md5(sRecvPath);
    if (NULL == psReceivedMd5 || 0 != strncmp(psReceivedMd5, psExpectedMd5, 32)) {
        send_ack_md5(nClientFd, sOrigin, sSenderRealm, "CHECK_KO", pstMaester->realm_name);
        free(psReceivedMd5);
        return;
    }
    free(psReceivedMd5);
    send_ack_md5(nClientFd, sOrigin, sSenderRealm, "CHECK_OK", pstMaester->realm_name);

    /* Parse the order file into an aggregated list before locking. */
    {
        struct OrderLine stAgg[256];
        int nAggCount = 0;
        int nOrderFd  = open(sRecvPath, O_RDONLY);
        char sLine[256];
        int nPos = 0;
        char cCh;
        int nR;

        if (-1 == nOrderFd) {
            psRejectReason = "REJECT&UNKNOWN_PRODUCT";
        } else {
            /* Parse the received text file line by line using read(). */
            while (NULL == psRejectReason) {
                nR = (int)read(nOrderFd, &cCh, 1);
                if (nR <= 0) {
                    break;
                }
                if (cCh == '\n' || nPos == (int)sizeof(sLine) - 1) {
                    sLine[nPos] = '\0';
                    nPos = 0;
                    if (sLine[0] != '\0') {
                        psRejectReason = aggregate_order_line(sLine, stAgg, &nAggCount);
                    }
                } else {
                    sLine[nPos++] = cCh;
                }
            }
            /* flush last line if not newline-terminated */
            if (NULL == psRejectReason && nPos > 0) {
                sLine[nPos] = '\0';
                psRejectReason = aggregate_order_line(sLine, stAgg, &nAggCount);
            }
            close(nOrderFd);
        }

        /* Lock, re-read current products pointer, validate, then apply */
        /* Lock before checking and subtracting stock so concurrent trades
         * cannot both consume the same units. */
        pthread_mutex_lock(&g_data_mutex);
        {
            Product *pstProducts = *ppstProducts;
            int nTotalProducts = *pnTotal;
            int k;

            // Validate the full aggregated list inside the same lock that will
            // apply the subtractions. A two-phase approach (validate outside,
            // lock-and-apply inside) would have a TOCTOU race: a concurrent
            // incoming trade could drain stock between the check and the write.
            if (NULL == psRejectReason) {
                /* Validate everything before applying any subtraction. */
                for (k = 0; k < nAggCount && NULL == psRejectReason; k++) {
                    int i;
                    int nFound = 0;

                    for (i = 0; i < nTotalProducts; i++) {
                        if (0 == strcasecmp(pstProducts[i].name, stAgg[k].name)) {
                            nFound = 1;
                            if (pstProducts[i].amount < stAgg[k].amount)
                                psRejectReason = "REJECT&OUT_OF_STOCK";
                            break;
                        }
                    }
                    if (!nFound) {
                        psRejectReason = "REJECT&UNKNOWN_PRODUCT";
                    }
                }
            }

            if (NULL != psRejectReason) {
                Frame stResp;

                pthread_mutex_unlock(&g_data_mutex);
                build_frame(&stResp, MSG_ORDER_RESP, sOrigin, sSenderRealm, (char *)psRejectReason, (short)strlen(psRejectReason));
                send_frame(nClientFd, &stResp);
                nLen = asprintf(&psOutput, "\n>>> Trade rejected: %s\n", psRejectReason + 7);
                if (-1 != nLen) {
                    printF(psOutput);
                    free(psOutput);
                }
                return;
            }

            // Reaching here means every line passed validation under the lock.
            // All subtractions are performed before releasing the lock, so
            // there is no window in which a partial order is visible to a
            // concurrent trade — either all units are debited or none are.
            /* Apply aggregated changes */
            /* All lines are valid, so the order is applied atomically. */
            for (k = 0; k < nAggCount; k++) {
                apply_stock_delta(pstProducts, nTotalProducts, stAgg[k].name, -stAgg[k].amount);
            }

            if (NULL != pstMaester->stock_path) {
                write_stock_file(pstMaester->stock_path, pstProducts, nTotalProducts);
            }
        }
        pthread_mutex_unlock(&g_data_mutex);
    }

    {
        Frame stResp;
        build_frame(&stResp, MSG_ORDER_RESP, sOrigin, sSenderRealm, "OK", 2);
        send_frame(nClientFd, &stResp);
    }
    printF("\n>>> Trade request processed. Order fulfilled. Stock updated.\n");
}

/* ── send_trade_request ──────────────────────────────────────────────────── */

/********************
 *
 * @Name: send_trade_request
 * @Def: Sends a trade order to an allied realm.
 *       Protocol:
 *         1. Write order to temp file ("PRODUCT&AMOUNT\n" per line)
 *         2. Compute file MD5 and size
 *         3. Build 0x14 header: "trade_list.txt&size&md5"
 *         4. Connect directly to ally; send 0x14
 *         5. Wait for 0x31 OK
 *         6. Send order file in 0x15 frames
 *         7. Wait for 0x32 ACK MD5
 *         8. Wait for 0x16 order response
 *         9. On OK: apply stock delta (+gain) and persist
 * @Arg: In:     psRealm        = allied realm
 *       In:     pstMaester     = local Maester config
 *       In/Out: ppstProducts   = local product array (updated on OK)
 *       In/Out: pnTotalProducts = number of local products
 *       In:     pstItems       = trade items selected by user
 *       In:     nItemCount     = number of trade items
 *       In/Out: ppstCache      = remote inventory cache
 *       In/Out: pnCacheCount   = number of cache entries
 *       In:     nEnvoyIdx      = envoy index (for unique temp file name)
 * @Ret: 0 on success, -1 on failure
 *
 ********************/
int send_trade_request(char *psRealm, Maester *pstMaester, Product **ppstProducts, int *pnTotalProducts, TradeItem *pstItems, int nItemCount, RemoteInventory **ppstCache, int *pnCacheCount, int nEnvoyIdx) {
    char sOrigin[ORIGIN_SIZE];
    char sAIp[IP_SIZE];
    int nAPort;
    char sTradePath[512];
    int nFileSize;
    char *psMd5     = NULL;
    char sData[DATA_SIZE];
    short nDataLen;
    Frame stFrame;
    Frame stAck;
    Frame stResp;
    int nFd;
    char *psOutput  = NULL;
    int nLen;
    int i;

    (void)ppstProducts;
    (void)pnTotalProducts;
    (void)ppstCache;
    (void)pnCacheCount;

    /* Outgoing trades require an accepted alliance. */
    if (PLEDGE_ALLIED != get_pledge_status(psRealm)) {
        printF("Not allied with this realm. Cannot trade.\n");
        return -1;
    }

    /* Trade uses the direct address learned during PLEDGE. */
    if (0 != get_pledge_ip_port(psRealm, sAIp, &nAPort)) {
        nLen = asprintf(&psOutput, "Cannot find address for %s.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return -1;
    }

    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    /* Write order file — use nEnvoyIdx for unique name. */
    if (NULL != pstMaester->user_dir) {
        if (0 != ensure_dir(pstMaester->user_dir)) {
            printF("Cannot create trade directory.\n");
            return -1;
        }
        snprintf(sTradePath, sizeof(sTradePath), "%s/trade_send_%d.txt", pstMaester->user_dir, nEnvoyIdx);
    } else {
        snprintf(sTradePath, sizeof(sTradePath), "trade_send_%d.txt", nEnvoyIdx);
    }

    {
        int nTfd = open(sTradePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (-1 == nTfd) {
            printF("Cannot create trade order file.\n");
            return -1;
        }
        /* One selected item per line: product name and requested amount. */
        for (i = 0; i < nItemCount; i++) {
            char *psLine = NULL;
            int   nLlen  = asprintf(&psLine, "%s&%d\n", pstItems[i].name, pstItems[i].amount);
            if (-1 != nLlen) {
                write(nTfd, psLine, (size_t)nLlen);
                free(psLine);
            }
        }
        close(nTfd);
    }

    nFileSize = get_file_size(sTradePath);
    if (nFileSize < 0) {
        printF("Cannot read trade order file.\n");
        return -1;
    }

    psMd5 = compute_file_md5(sTradePath);
    if (NULL == psMd5) {
        printF("Cannot compute trade order checksum.\n");
        return -1;
    }

    nDataLen = (short)snprintf(sData, DATA_SIZE, "trade_list.txt&%d&%s", nFileSize, psMd5);
    free(psMd5);

    build_frame(&stFrame, MSG_ORDER_HEADER, sOrigin, psRealm, sData, nDataLen);

    nFd = connect_to_realm(sAIp, nAPort);
    if (nFd < 0) {
        nLen = asprintf(&psOutput, "Could not connect to %s.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return -1;
    }

    /* Send 0x14 header */
    if (send_frame(nFd, &stFrame) < 0) {
        close(nFd);
        return -1;
    }

    /* Wait for 0x31 OK */
    if (0 != recv_validated(nFd, &stAck, pstMaester) ||
        MSG_ACK_FILE != stAck.type) {
        printF("No valid ACK from trade partner.\n");
        close(nFd);
        return -1;
    }
    if (stAck.data_length >= 2 &&
        stAck.data[0] == 'K' && stAck.data[1] == 'O') {
        printF("Trade partner not ready (KO). Trade aborted.\n");
        close(nFd);
        return -1;
    }

    /* Send order file in 0x15 frames */
    if (0 != send_file_in_frames(nFd, sTradePath, MSG_ORDER_DATA, sOrigin, psRealm)) {
        printF("Error sending order file.\n");
        unlink(sTradePath);
        close(nFd);
        return -1;
    }
    /* File sent; no longer needed. */
    unlink(sTradePath);

    /* Wait for 0x32 ACK MD5 */
    if (0 != recv_validated(nFd, &stAck, pstMaester) || MSG_ACK_MD5 != stAck.type) {
        printF("No valid MD5 ACK from trade partner.\n");
        close(nFd);
        return -1;
    }
    if (stAck.data_length >= 8 &&
        0 == strncmp(stAck.data, "CHECK_KO", 8)) {
        printF("Trade partner reported bad checksum. Trade aborted.\n");
        close(nFd);
        return -1;
    }

    /* Wait for 0x16 order response */
    if (0 != recv_validated(nFd, &stResp, pstMaester) ||
        MSG_ORDER_RESP != stResp.type) {
        printF("No valid order response from trade partner.\n");
        close(nFd);
        return -1;
    }
    close(nFd);

    if (stResp.data_length >= 2 &&
        stResp.data[0] == 'O' && stResp.data[1] == 'K') {
        // The buyer's stock gain (+amount of each item purchased) is NOT
        // applied here in the child process. Forked children have a private
        // copy of the product array; any write would be lost when the child
        // exits. The parent applies the delta via apply_envoy_result() after
        // reading PIPE_TRADE_OK from the result pipe, under g_data_mutex.
        /* Stock update is applied in the parent via apply_envoy_result
         * so concurrent incoming trades are not overwritten. */
        nLen = asprintf(&psOutput, ">>> Order accepted by %s. Stock updated.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return 0;
    } else {
        char sDc[DATA_SIZE];
        char *psReason;

        memcpy(sDc, stResp.data, DATA_SIZE - 1);
        sDc[DATA_SIZE - 1] = '\0';
        psReason = strchr(sDc, '&');
        if (NULL != psReason) {
            psReason = psReason + 1;
        } else {
            psReason = sDc;
        }
        nLen = asprintf(&psOutput, ">>> Order rejected by %s: %s\n", psRealm, psReason);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return -1;
    }
}
