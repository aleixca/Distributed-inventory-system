/***********************************************
 *
 * @File:    message_handler.c
 * @Purpose: Dispatches incoming protocol frames to the appropriate handler and implements shared helper functions.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "message_handler.h"
#include "network.h"
#include "sigil.h"
#include "relay.h"
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
#include "handler_helpers.h"

/* ── Internal helpers ───────────────────────────────────────────────────── */

/* Parse "FIELD0&FIELD1&FIELD2&FIELD3" from a frame data buffer.
 * Fills up to 4 pointers (NULL for unused slots).
 * psDataCopy must be DATA_SIZE bytes; modified in-place by strtok. */
void parse_data_fields(char *psDataCopy, int nDataLen, char **ppsF0, char **ppsF1, char **ppsF2, char **ppsF3) {
    int nN;

    /* Respect data_length when valid; otherwise fall back to safe terminator. */
    if (nDataLen > 0 && nDataLen < DATA_SIZE) {
        nN = nDataLen;
    } else {
        nN = DATA_SIZE - 1;
    }
    psDataCopy[nN] = '\0';
    if (NULL != ppsF0) {
        *ppsF0 = NULL;
    }
    if (NULL != ppsF1) {
        *ppsF1 = NULL;
    }
    if (NULL != ppsF2) {
        *ppsF2 = NULL;
    }
    if (NULL != ppsF3) {
        *ppsF3 = NULL;
    }
    // strtok replaces each '&' separator with '\0' in-place, so the returned
    // pointers are direct offsets into psDataCopy — no heap allocation needed.
    // Callers must NOT free these pointers and must not use them after
    // psDataCopy goes out of scope. That is why every caller passes a local
    // sDataCopy[] array, not the frame's own data[] field.
    /* strtok replaces '&' with '\0', so returned pointers live in psDataCopy. */
    if (NULL != ppsF0) {
        *ppsF0 = strtok(psDataCopy, "&");
        if (NULL == *ppsF0) {
            return;
        }
    }
    if (NULL != ppsF1) {
        *ppsF1 = strtok(NULL, "&");
        if (NULL == *ppsF1) {
            return;
        }
    }
    if (NULL != ppsF2) {
        *ppsF2 = strtok(NULL, "&");
        if (NULL == *ppsF2) {
            return;
        }
    }
    if (NULL != ppsF3) {
        *ppsF3 = strtok(NULL, "&");
    }
}

/* Build and send a 0x31 ACK FILE frame on nFd.
 * Annex II: ORIGIN and DESTINATION must be empty; realm name goes in DATA. */
void send_ack_file(int nFd, char *psOriginStr, char *psDestRealm, char *psResultStr, char *psOurRealm) {
    Frame stF;
    char  sData[DATA_SIZE];
    short nDlen;

    /* ACK FILE is used as "ready/not ready to receive the file". */
    nDlen = (short)snprintf(sData, DATA_SIZE, "%s&%s", psResultStr, psOurRealm);
    (void)psOriginStr; (void)psDestRealm;
    build_frame(&stF, MSG_ACK_FILE, "", "", sData, nDlen);
    send_frame(nFd, &stF);
}

/* Build and send a 0x32 ACK MD5 frame on nFd.
 * Annex II: ORIGIN and DESTINATION must be empty; realm name goes in DATA. */
void send_ack_md5(int nFd, char *psOriginStr, char *psDestRealm, char *psResultStr, char *psOurRealm) {
    Frame stF;
    char  sData[DATA_SIZE];
    short nDlen;

    /* ACK MD5 reports whether the received file matched its announced hash. */
    nDlen = (short)snprintf(sData, DATA_SIZE, "%s&%s", psResultStr, psOurRealm);
    (void)psOriginStr;
    (void)psDestRealm;
    build_frame(&stF, MSG_ACK_MD5, "", "", sData, nDlen);
    send_frame(nFd, &stF);
}

/* Search the pledge table for a realm whose stored IP:Port matches psOriginField.
 * Returns the realm name string (static storage in pledge table) or NULL. */
char *find_realm_by_origin(char *psOriginField) {
    char  sIp[IP_SIZE];
    int   nPort;
    int   i;
    int   nN;
    char *psR;
    char  sRip[IP_SIZE];
    int   nRport;

    /* Convert frame ORIGIN into IP/port, then compare against pledge table. */
    if (0 != parse_origin(psOriginField, sIp, &nPort)) {
        return NULL;
    }
    nN = get_pledge_count();
    for (i = 0; i < nN; i++) {
        psR = get_pledge_realm(i);
        if (NULL == psR) {
            continue;
        }
        if (0 != get_pledge_ip_port(psR, sRip, &nRport)) {
            continue;
        }
        if (0 == strncmp(sRip, sIp, IP_SIZE) && nRport == nPort) {
            return psR;
        }
    }
    return NULL;
}

/* Create directory if it does not already exist.
 * Returns 0 on success or if it already exists, -1 on real failure. */
int ensure_dir(const char *psPath) {
    if (0 != mkdir(psPath, 0755) && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Receive a frame and validate its checksum.
 * On checksum failure sends an inline NACK on nFd and returns -1.
 * Returns 0 on success, -1 on any error. */
int recv_validated(int nFd, Frame *pstF, Maester *pstM) {
    char  sOrigin[ORIGIN_SIZE];
    char  sNackData[DATA_SIZE];
    short nDl;
    Frame stNack;

    // Sending NACK inline before returning -1 is intentional: the peer is
    // blocked in its own recv_validated call waiting for our next frame.
    // A silent -1 return would leave it waiting until its socket timeout.
    // The NACK unblocks it immediately so both sides can abort cleanly.
    /* Common receive path for multi-frame protocols. */
    if (0 != recv_frame(nFd, pstF)) {
        return -1;
    }
    if (!validate_frame(pstF)) {
        format_origin(sOrigin, pstM->listen_ip, pstM->listen_port);
        nDl = (short)snprintf(sNackData, DATA_SIZE, "%s", pstM->realm_name);
        build_frame(&stNack, MSG_NACK, sOrigin, "", sNackData, nDl);
        send_frame(nFd, &stNack);
        return -1;
    }
    return 0;
}

/* Send a NACK frame inline on an already-open nFd. */
void send_nack_on_fd(int nFd, Maester *pstM) {
    char  sOrigin[ORIGIN_SIZE];
    char  sData[DATA_SIZE];
    short nDl;
    Frame stNack;

    format_origin(sOrigin, pstM->listen_ip, pstM->listen_port);
    nDl = (short)snprintf(sData, DATA_SIZE, "%s", pstM->realm_name);
    build_frame(&stNack, MSG_NACK, sOrigin, "", sData, nDl);
    send_frame(nFd, &stNack);
}

/* ── Dispatcher ─────────────────────────────────────────────────────────── */

/********************
 *
 * @Name: handle_incoming
 * @Def: Dispatches an already-received frame to the appropriate handler.
 *       The frame's checksum has already been validated by the caller.
 * @Arg: In:     pstFrame        = received and validated frame
 *       In:     nClientFd       = open connection (may be used to send responses)
 *       In:     pstMaester      = local Maester config
 *       In/Out: ppstProducts    = local product array
 *       In/Out: pnTotalProducts = number of local products
 * @Ret: None
 *
 ********************/
void handle_incoming(Frame *pstFrame, int nClientFd, Maester *pstMaester, Product **ppstProducts, int *pnTotalProducts) {
    switch (pstFrame->type) {

        case MSG_ALLIANCE_HEADER:
            // A single handler entry point covers both roles: final destination
            // and intermediate hop. The DESTINATION field in the frame header
            // is the canonical realm name of the intended recipient. If it
            // matches us we run the full sigil-receive handshake; otherwise we
            // forward the frame one hop closer using the routing table.
            /* 0x01: are we the destination or an intermediate hop? */
            if (0 == strcasecmp(pstFrame->destination, pstMaester->realm_name)) {
                handle_alliance_dest(pstFrame, nClientFd, pstMaester);
            } else {
                relay_pledge_hop(pstFrame, nClientFd, pstMaester);
            }
            break;

        case MSG_ALLIANCE_RESP:
            /* 0x03: ACCEPT or REJECT delivered directly by the responding realm */
            handle_alliance_resp(pstFrame, nClientFd, pstMaester);
            break;

        case MSG_LIST_REQUEST:
            /* 0x11: ally wants our product list */
            handle_list_request(pstFrame, nClientFd, pstMaester, ppstProducts, pnTotalProducts);
            break;

        case MSG_ORDER_HEADER:
            /* 0x14: ally is sending a trade order */
            handle_order_header(pstFrame, nClientFd, pstMaester, ppstProducts, pnTotalProducts);
            break;

        case MSG_DISCONNECT:
            /* 0x27: ally is shutting down */
            handle_disconnect(pstFrame);
            break;

        case MSG_UNKNOWN_REALM:
            /* 0x21: an intermediate hop couldn't find a route to our destination */
            {
                char  sDc[DATA_SIZE];
                char *psDestName;
                char *psOutput;
                int   nLen;

                memcpy(sDc, pstFrame->data, DATA_SIZE - 1);
                sDc[DATA_SIZE - 1] = '\0';
                psDestName = strchr(sDc, '&');
                if (NULL != psDestName) {
                    psDestName = psDestName + 1;
                } else {
                    psDestName = sDc;
                }
                nLen = asprintf(&psOutput, "\n>>> UNKNOWN_REALM: no route to '%s' found by hop at %s.\n", psDestName, pstFrame->origin);
                if (-1 != nLen) {
                    printF(psOutput);
                    free(psOutput);
                }
            }
            break;

        case MSG_NACK:
            /* 0x69: peer rejected a frame we sent (bad checksum) */
            printF("[NACK] A peer rejected our last frame (checksum error).\n");
            break;

        default:
            // Silently dropping an unknown frame type would leave the sender
            // blocked in recv_validated waiting for a reply that never comes.
            // Sending NACK gives the peer a defined signal to abort its current
            // operation rather than hanging until its socket timeout fires.
            /* Unknown or unsolicited frame type — send NACK per protocol */
            send_nack_on_fd(nClientFd, pstMaester);
            break;
    }
}

/* ── DISCONNECT (0x27) ───────────────────────────────────────────────────── */

/********************
 *
 * @Name: handle_disconnect
 * @Def: Marks the sending realm as INACTIVE in the pledge table.
 *       Identifies the sender by matching ORIGIN IP against stored pledges.
 * @Arg: In: pstFrame = received 0x27 frame
 * @Ret: None
 *
 ********************/
void handle_disconnect(Frame *pstFrame) {
    char  sDIp[IP_SIZE];
    int   nDPort;
    char *psOutput = NULL;
    int   nLen;
    int   i;
    int   nCount = get_pledge_count();

    if (0 != parse_origin(pstFrame->origin, sDIp, &nDPort)) {
        printF("[DISCONNECT] Could not parse origin — ignoring.\n");
        return;
    }

    for (i = 0; i < nCount; i++) {
        char *psRealm = get_pledge_realm(i);
        char  sStoredIp[IP_SIZE];
        int   nStoredPort;

        if (NULL == psRealm) {
            continue;
        }
        if (0 != get_pledge_ip_port(psRealm, sStoredIp, &nStoredPort)) {
            continue;
        }
        if (0 == strncmp(sStoredIp, sDIp, IP_SIZE) && nStoredPort == nDPort) {
            update_pledge_status(psRealm, PLEDGE_INACTIVE);
            nLen = asprintf(&psOutput, "\n[DISCONNECT] Realm %s has left the realm.\n", psRealm);
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
            return;
        }
    }

    /* Fallback: use destination field or just log */
    printF("\n[DISCONNECT] An allied realm has disconnected.\n");
}
