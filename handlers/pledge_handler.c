/***********************************************
 *
 * @File:    pledge_handler.c
 * @Purpose: Implements alliance request sending, receiving, and response handling for the PLEDGE protocol.
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

/* ── ALLIANCE DESTINATION (0x01 at destination) ─────────────────────────── */

/********************
 *
 * @Name: handle_alliance_dest
 * @Def: Handles an incoming 0x01 ALLIANCE HEADER frame when we are the
 *       final destination.  Full handshake:
 *         1. Parse DATA: "SenderRealm&SigilFile&Size&MD5"
 *         2. Send 0x31 OK to sender on nClientFd
 *         3. Receive sigil file in 0x02 frames → save to user_dir/SigilFile
 *         4. Verify MD5
 *         5. Send 0x32 CHECK_OK or CHECK_KO
 *         6. On OK: record incoming pledge, prompt user
 * @Arg: In: pstFrame   = received 0x01 frame
 *       In: nClientFd  = connection from sender (still open)
 *       In: pstMaester = local Maester config
 * @Ret: None
 *
 ********************/
void handle_alliance_dest(Frame *pstFrame, int nClientFd, Maester *pstMaester) {
    char  sDataCopy[DATA_SIZE];
    char *psAName       = NULL;
    char *psSigilFileRaw = NULL;
    char *psSizeStr     = NULL;
    char *psExpectedMd5 = NULL;
    int   nFileSize;
    char  sOrigin[ORIGIN_SIZE];
    char  sAIp[IP_SIZE];
    int   nAPort;
    char *psSavePath    = NULL;
    char *psReceivedMd5 = NULL;
    char *psOutput      = NULL;
    int   nLen;
    char *psSigilBasename;

    /* Work on a local copy because parse_data_fields() modifies the buffer. */
    memcpy(sDataCopy, pstFrame->data, DATA_SIZE - 1);
    sDataCopy[DATA_SIZE - 1] = '\0';
    parse_data_fields(sDataCopy, (int)pstFrame->data_length,
                      &psAName, &psSigilFileRaw, &psSizeStr, &psExpectedMd5);

    if (NULL == psAName || NULL == psSigilFileRaw ||
        NULL == psSizeStr || NULL == psExpectedMd5) {
        printF("[PLEDGE] Malformed 0x01 header — ignoring.\n");
        return;
    }

    /* The announced size controls how many 0x02 frames we expect. */
    nFileSize = atoi(psSizeStr);

    // Strip any directory prefix sent by the remote: we must save the sigil into
    // OUR user_dir only. Without this, a malicious DATA field could contain
    // "../../../etc/passwd" and overwrite arbitrary files via the save path.
    /* Use only the filename component, not any directory prefix */
    psSigilBasename = strrchr(psSigilFileRaw, '/');
    if (NULL != psSigilBasename) {
        psSigilBasename = psSigilBasename + 1;
    } else {
        psSigilBasename = psSigilFileRaw;
    }

    /* Parse sender's listen address from ORIGIN field */
    if (0 != parse_origin(pstFrame->origin, sAIp, &nAPort)) {
        printF("[PLEDGE] Could not parse origin address — ignoring.\n");
        return;
    }

    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    /* Send 0x31 OK: we are ready to receive the sigil */
    send_ack_file(nClientFd, sOrigin, psAName, "OK", pstMaester->realm_name);

    if (nFileSize <= 0) {
        /* Zero-size sigil: record pledge immediately */
        add_incoming_pledge(psAName, sAIp, nAPort);
        nLen = asprintf(&psOutput,
            "\n>>> Alliance request received from %s.\n", psAName);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return;
    }

    /* Ensure user directory exists */
    if (0 != ensure_dir(pstMaester->user_dir)) {
        send_ack_md5(nClientFd, sOrigin, psAName,
                     "CHECK_KO", pstMaester->realm_name);
        return;
    }

    nLen = asprintf(&psSavePath, "%s/%s", pstMaester->user_dir, psSigilBasename);
    if (-1 == nLen || NULL == psSavePath) {
        send_ack_md5(nClientFd, sOrigin, psAName,
                     "CHECK_KO", pstMaester->realm_name);
        return;
    }

    /* Receive sigil in 0x02 frames */
    {
        int nRc = recv_file_in_frames(nClientFd, psSavePath,
                                      nFileSize, MSG_SIGIL_DATA);
        if (RECV_FRAMES_CKSUM == nRc) {
            send_nack_on_fd(nClientFd, pstMaester);
            send_ack_md5(nClientFd, sOrigin, psAName,
                         "CHECK_KO", pstMaester->realm_name);
            free(psSavePath);
            return;
        }
        if (RECV_FRAMES_OK != nRc) {
            send_ack_md5(nClientFd, sOrigin, psAName,
                         "CHECK_KO", pstMaester->realm_name);
            free(psSavePath);
            return;
        }
    }

    /* Verify MD5 */
    /* MD5 is computed by the OS md5sum command through compute_file_md5(). */
    psReceivedMd5 = compute_file_md5(psSavePath);
    if (NULL == psReceivedMd5 ||
        0 != strncmp(psReceivedMd5, psExpectedMd5, 32)) {
        send_ack_md5(nClientFd, sOrigin, psAName,
                     "CHECK_KO", pstMaester->realm_name);
        if (NULL != psSavePath) {
            unlink(psSavePath);
        }
        free(psSavePath);
        free(psReceivedMd5);
        return;
    }
    free(psReceivedMd5);

    /* MD5 OK */
    send_ack_md5(nClientFd, sOrigin, psAName,
                 "CHECK_OK", pstMaester->realm_name);

    /* At this point the sigil file is correct, so the user can accept/reject. */
    add_incoming_pledge(psAName, sAIp, nAPort);

    nLen = asprintf(&psOutput,
        "\n>>> Alliance request received from %s.\n", psAName);
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }

    free(psSavePath);
}

/* ── ALLIANCE RESPONSE (0x03 at origin) ─────────────────────────────────── */

/********************
 *
 * @Name: handle_alliance_resp
 * @Def: Handles an incoming 0x03 ALLIANCE RESP frame (ACCEPT or REJECT)
 *       sent directly by the responding realm to our listen address.
 *       Sends 0x31 ACK back on the same nClientFd (responding realm waits).
 * @Arg: In: pstFrame   = received 0x03 frame
 *       In: nClientFd  = connection from responder (still open)
 *       In: pstMaester = local Maester config
 * @Ret: None
 *
 ********************/
void handle_alliance_resp(Frame *pstFrame, int nClientFd, Maester *pstMaester) {
    char  sDataCopy[DATA_SIZE];
    char *psResultStr = NULL;
    char *psBName     = NULL;
    char  sOrigin[ORIGIN_SIZE];
    char  sBIp[IP_SIZE];
    int   nBPort;
    char *psOutput    = NULL;
    int   nLen;

    /* Copy response DATA before tokenising ACCEPT/REJECT and realm name. */
    memcpy(sDataCopy, pstFrame->data, DATA_SIZE - 1);
    sDataCopy[DATA_SIZE - 1] = '\0';
    parse_data_fields(sDataCopy, (int)pstFrame->data_length,
                      &psResultStr, &psBName, NULL, NULL);

    if (NULL == psResultStr) {
        printF("[ALLIANCE_RESP] Malformed 0x03 — ignoring.\n");
        return;
    }

    /* The responding realm's listening address is in ORIGIN */
    if (0 != parse_origin(pstFrame->origin, sBIp, &nBPort)) {
        printF("[ALLIANCE_RESP] Could not parse origin address.\n");
        return;
    }

    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    /* Use psBName from data if present, else use destination field */
    if (NULL == psBName || psBName[0] == '\0') {
        psBName = pstFrame->destination;
    }

    // The responding realm doesn't know our pledge timed out on our side.
    // Sending NACK (not just dropping) tells the responder the handshake is
    // over so its recv_validated call returns -1 instead of hanging until
    // its own 30-second socket timeout expires.
    /* If the pledge already timed out (FAILED) or was never sent, send NACK
     * per protocol (alliance discarded due to timeout) and stop. */
    {
        int nCur = get_pledge_status(psBName);
        if (PLEDGE_OUTGOING_PENDING != nCur) {
            nLen = asprintf(&psOutput,
                "\n[PLEDGE] Late response from %s ignored "
                "(pledge already expired or was not pending).\n", psBName);
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
            send_nack_on_fd(nClientFd, pstMaester);
            return;
        }
    }

    if (0 == strncmp(psResultStr, "ACCEPT", 6)) {
        /* ACCEPT stores the direct address for future LIST/TRADE operations. */
        update_pledge_status(psBName, PLEDGE_ALLIED);
        update_pledge_ip_port(psBName, sBIp, nBPort);

        nLen = asprintf(&psOutput,
            "\n>>> Alliance with %s forged successfully!\n", psBName);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }

        /* Send 0x31 OK to confirm alliance receipt */
        send_ack_file(nClientFd, sOrigin, psBName, "OK", pstMaester->realm_name);

    } else {
        /* REJECT */
        update_pledge_status(psBName, PLEDGE_FAILED);

        nLen = asprintf(&psOutput,
            "\n>>> Alliance with %s was refused!\n", psBName);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }

        /* Still send 0x31 KO so responder can close cleanly */
        send_ack_file(nClientFd, sOrigin, psBName, "KO", pstMaester->realm_name);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  OUTGOING OPERATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── send_pledge ─────────────────────────────────────────────────────────── */

/********************
 *
 * @Name: send_pledge
 * @Def: Initiates an alliance request to psRealm using the given sigil file.
 *       Protocol:
 *         1. Guards: already-allied check, sigil ownership, sigil readable
 *         2. Compute sigil MD5 and size
 *         3. Build 0x01 with DATA "OurName&SigilFile&Size&MD5"
 *         4. Connect to next hop via routing table
 *         5. Send 0x01; wait for 0x31 OK
 *         6. Send sigil in 0x02 frames
 *         7. Wait for 0x32 ACK MD5
 *         8. Print confirmation
 * @Arg: In: psRealm    = target realm name
 *       In: psSigil    = path to sigil file
 *       In: pstMaester = local Maester config
 * @Ret: 0 on success, -1 on any failure
 *
 ********************/
int send_pledge(char *psRealm, char *psSigil, Maester *pstMaester) {
    char  sOrigin[ORIGIN_SIZE];
    char  sIp[IP_SIZE];
    int   nPort;
    int   nFileSize;
    char *psMd5         = NULL;
    char *psBasenamePtr = NULL;
    char  sData[DATA_SIZE];
    short nDataLen;
    Frame stFrame;
    Frame stAck;
    int   nFd;
    char *psOutput      = NULL;
    int   nLen;

    /* Cannot pledge a realm that is already allied. */
    if (PLEDGE_ALLIED == get_pledge_status(psRealm)) {
        nLen = asprintf(&psOutput,
            "The alliance with %s is already forged. No further pledge is needed.\n",
            psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return -1;
    }

    // Require the separator character after the prefix so that a path like
    // "realms/arryn-evil/sigil.jpg" cannot pass a naive strncmp against
    // "realms/arryn" — the prefix matches but the next byte is '-', not '/'.
    /* Sigil must belong to this Maester's own house directory. */
    if (NULL != pstMaester->user_dir) {
        size_t nDirLen = strlen(pstMaester->user_dir);
        if (0 != strncmp(psSigil, pstMaester->user_dir, nDirLen) ||
            (psSigil[nDirLen] != '/' && psSigil[nDirLen] != '\\')) {
            printF("Honour demands your own house sigil. The pledge is hereby withdrawn.\n");
            return -1;
        }
    }

    /* Sigil file must be readable */
    {
        int nTestFd = open(psSigil, O_RDONLY);
        if (-1 == nTestFd) {
            printF("Sigil not found. The pledge is hereby withdrawn.\n");
            return -1;
        }
        close(nTestFd);
    }

    nFileSize = get_file_size(psSigil);
    if (nFileSize <= 0) {
        printF("Sigil file is empty or unreadable. The pledge is hereby withdrawn.\n");
        return -1;
    }

    /* Hash the local sigil before announcing it in the 0x01 header. */
    psMd5 = compute_file_md5(psSigil);
    if (NULL == psMd5) {
        printF("Cannot compute sigil checksum.\n");
        return -1;
    }

    // Send only the bare filename in the 0x01 header DATA, not the full local
    // path. The receiver uses it to build its own save path under its user_dir.
    // Sending the full path would expose our directory layout and could
    // confuse the receiver's path construction.
    /* Extract filename component only */
    psBasenamePtr = strrchr(psSigil, '/');
    if (NULL == psBasenamePtr) {
        psBasenamePtr = strrchr(psSigil, '\\');
    }
    if (NULL != psBasenamePtr) {
        psBasenamePtr = psBasenamePtr + 1;
    } else {
        psBasenamePtr = psSigil;
    }

    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    nDataLen = (short)snprintf(sData, DATA_SIZE, "%s&%s&%d&%s",
                               pstMaester->realm_name, psBasenamePtr,
                               nFileSize, psMd5);
    free(psMd5);

    build_frame(&stFrame, MSG_ALLIANCE_HEADER, sOrigin, psRealm,
                sData, nDataLen);

    /* Route lookup */
    if (0 != lookup_route(psRealm, pstMaester, sIp, &nPort)) {
        printF("No route to realm. The pledge is hereby withdrawn.\n");
        return -1;
    }

    /* Connect to the next hop, not necessarily the final destination. */
    nFd = connect_to_realm(sIp, nPort);
    if (nFd < 0) {
        nLen = asprintf(&psOutput,
                        "Could not connect to route for %s.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return -1;
    }

    /* Send 0x01 header */
    if (send_frame(nFd, &stFrame) < 0) {
        close(nFd);
        return -1;
    }

    // After sending 0x01 the connection stays open for the entire handshake:
    // 0x01 → 0x31 ACK → 0x02 chunks → 0x32 ACK MD5.
    // An intermediate hop may reply with 0x21 UNKNOWN_REALM instead of 0x31
    // if it has no route to the destination, so we check for that explicitly.
    /* Wait for 0x31 ACK FILE (or 0x21 UNKNOWN_REALM from intermediate hop) */
    if (0 != recv_validated(nFd, &stAck, pstMaester)) {
        printF("No valid ACK received from destination. Pledge aborted.\n");
        close(nFd);
        return -1;
    }
    if (MSG_UNKNOWN_REALM == stAck.type) {
        char  sDc[DATA_SIZE];
        char *psDestName;

        memcpy(sDc, stAck.data, DATA_SIZE - 1);
        sDc[DATA_SIZE - 1] = '\0';
        psDestName = strchr(sDc, '&');
        if (NULL != psDestName) {
            psDestName = psDestName + 1;
        } else {
            psDestName = psRealm;
        }
        nLen = asprintf(&psOutput,
            ">>> No route to '%s' — intermediate hop could not forward pledge.\n",
            psDestName);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        close(nFd);
        return -1;
    }
    if (MSG_ACK_FILE != stAck.type) {
        printF("Unexpected response from destination. Pledge aborted.\n");
        close(nFd);
        return -1;
    }
    if (stAck.data_length >= 2 &&
        stAck.data[0] == 'K' && stAck.data[1] == 'O') {
        printF("Destination rejected pledge (KO). Pledge aborted.\n");
        close(nFd);
        return -1;
    }

    /* Send sigil in 0x02 frames */
    if (0 != send_file_in_frames(nFd, psSigil, MSG_SIGIL_DATA,
                                  sOrigin, psRealm)) {
        printF("Error sending sigil file.\n");
        close(nFd);
        return -1;
    }

    /* Wait for 0x32 ACK MD5 */
    /* The final destination reports whether the received sigil hash matched. */
    if (0 != recv_validated(nFd, &stAck, pstMaester) ||
        MSG_ACK_MD5 != stAck.type) {
        printF("No valid MD5 ACK from destination.\n");
        close(nFd);
        return -1;
    }
    close(nFd);

    if (stAck.data_length >= 8 &&
        0 == strncmp(stAck.data, "CHECK_KO", 8)) {
        printF("Destination reported bad MD5. Pledge failed.\n");
        return -1;
    }

    nLen = asprintf(&psOutput,
        ">>> Pledge dispatched to %s. Awaiting their response.\n", psRealm);
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }
    return 0;
}

/* ── send_pledge_response ────────────────────────────────────────────────── */

/********************
 *
 * @Name: send_pledge_response
 * @Def: Responds to an incoming PLEDGE (user typed PLEDGE RESPOND X ACCEPT/REJECT).
 *       Protocol:
 *         1. Verify INCOMING_PENDING pledge exists
 *         2. Build 0x03 with DATA "ACCEPT&OurRealm" or "REJECT&OurRealm"
 *         3. Connect directly to originator's listen address
 *         4. Send 0x03; wait for 0x31 ACK from originator
 *         5. Return result status
 * @Arg: In: psRealm    = realm to respond to
 *       In: psResponse = "ACCEPT" or "REJECT"
 *       In: pstMaester = local Maester config
 * @Ret: PLEDGE_ALLIED on success, PLEDGE_FAILED otherwise
 *
 ********************/
int send_pledge_response(char *psRealm, char *psResponse, Maester *pstMaester) {
    char  sOrigin[ORIGIN_SIZE];
    char  sAIp[IP_SIZE];
    int   nAPort;
    char  sData[DATA_SIZE];
    short nDataLen;
    Frame stFrame;
    Frame stAck;
    int   nFd;
    char *psOutput = NULL;
    int   nLen;

    /* We can only respond to a pledge that has reached us and is still pending. */
    if (PLEDGE_INCOMING_PENDING != get_pledge_status(psRealm)) {
        nLen = asprintf(&psOutput,
            "No incoming pledge from %s to respond to.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return PLEDGE_FAILED;
    }

    /* Incoming pledge stored the originator's direct address. */
    if (0 != get_pledge_ip_port(psRealm, sAIp, &nAPort)) {
        nLen = asprintf(&psOutput, "Cannot find address for %s.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return PLEDGE_FAILED;
    }

    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    nDataLen = (short)snprintf(sData, DATA_SIZE, "%s&%s",
                               psResponse, pstMaester->realm_name);
    build_frame(&stFrame, MSG_ALLIANCE_RESP, sOrigin, psRealm,
                sData, nDataLen);

    nFd = connect_to_realm(sAIp, nAPort);
    if (nFd < 0) {
        nLen = asprintf(&psOutput,
            "Could not connect to %s to send response.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return PLEDGE_FAILED;
    }

    if (send_frame(nFd, &stFrame) < 0) {
        close(nFd);
        return PLEDGE_FAILED;
    }

    /* Wait for 0x31 ACK from originator */
    recv_validated(nFd, &stAck, pstMaester);
    close(nFd);

    if (0 == strncmp(psResponse, "ACCEPT", 6)) {
        if (MSG_ACK_FILE == stAck.type &&
            stAck.data_length >= 2 &&
            stAck.data[0] == 'O' && stAck.data[1] == 'K') {
            nLen = asprintf(&psOutput,
                "Alliance with %s established.\n", psRealm);
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
            return PLEDGE_ALLIED;
        } else {
            nLen = asprintf(&psOutput,
                "Alliance with %s failed (origin timed out or rejected).\n",
                psRealm);
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
            return PLEDGE_FAILED;
        }
    } else {
        nLen = asprintf(&psOutput, "REJECT sent to %s.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return PLEDGE_FAILED;
    }
}
