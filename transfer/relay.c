/***********************************************
 *
 * @File:    relay.c
 * @Purpose: Implements intermediate-hop relay logic for PLEDGE frames between realms.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "relay.h"
#include "network.h"
#include "sigil.h"
#include "io.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/********************
 *
 * @Name: relay_pledge_hop
 * @Def: Handles a PLEDGE (0x01) frame at an intermediate node.
 *       Relays the header and the entire sigil-file exchange between
 *       the origin (nClientFd) and the next hop (nFd2), without decoding
 *       or verifying the file — intermediate nodes just forward.
 *
 * Protocol sequence performed:
 *   nClientFd (A)              us (B)               nFd2 (C/next-hop)
 *      [0x01 already received]
 *                  ── 0x01 ──────────────────────────────────────►
 *                  ◄─ 0x31 (ACK FILE) ─────────────────────────────
 *                  ── 0x31 ──────────────────►  (relay back to A)
 *                  ── 0x02 frames ──────────────────────────────────►
 *                  ◄─ 0x32 (ACK MD5) ──────────────────────────────
 *                  ── 0x32 ──────────────────►  (relay back to A)
 *
 * @Arg: In: pstHdr     = already-received 0x01 frame
 *       In: nClientFd  = connection to the previous hop (or origin)
 *       In: pstMaester = local Maester config (for route lookup)
 * @Ret: None
 *
 ********************/
void relay_pledge_hop(Frame *pstHdr, int nClientFd, Maester *pstMaester) {
    char   sIp[IP_SIZE];
    int    nPort;
    int    nFd2;
    char  *psOutput  = NULL;
    int    nLen;
    Frame  stAck;
    int    nFileSize = 0;

    /* Parse DATA: "SenderRealm&SigilName&FileSize&MD5SUM"
     * Extract field 0 (sender realm name) and field 2 (file size). */
    char sSenderRealm[DEST_SIZE];
    {
        char  sDataCopy[DATA_SIZE];
        char *psTok;
        int   nField = 0;

        /* Copy header DATA because strtok modifies it while extracting fields. */
        strncpy(sDataCopy, pstHdr->data, DATA_SIZE - 1);
        sDataCopy[DATA_SIZE - 1] = '\0';
        sSenderRealm[0] = '\0';
        psTok = strtok(sDataCopy, "&");
        /* The frame DATA carries "SenderRealm&SigilName&FileSize&MD5".
         * We only need fields 0 and 2 here; the rest are forwarded opaquely
         * so we don't need to decode or validate them at the relay hop. */
        while (NULL != psTok) {
            if (0 == nField) {
                strncpy(sSenderRealm, psTok, DEST_SIZE - 1);
                sSenderRealm[DEST_SIZE - 1] = '\0';
            }
            if (2 == nField) {
                nFileSize = atoi(psTok);
                break;
            }
            nField++;
            psTok = strtok(NULL, "&");
        }
    }

    /* Log the hop — show realm names, not raw IP:Port */
    {
        const char *psHopSrc;

        if (sSenderRealm[0]) {
            psHopSrc = sSenderRealm;
        } else {
            psHopSrc = pstHdr->origin;
        }
        nLen = asprintf(&psOutput, "\n>>> Received hop: %s -> %s (PLEDGE)\n",
                        psHopSrc, pstHdr->destination);
    }
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }

    /* No named route and no DEFAULT means truly unroutable. */
    if (0 != lookup_route(pstHdr->destination, pstMaester, sIp, &nPort)) {
        /* Send 0x21 UNKNOWN_REALM back to the previous hop/origin */
        Frame      stErrFrame;
        char       sOrigin[ORIGIN_SIZE];
        char       sEdata[DATA_SIZE];
        short      nEdlen;
        const char *psErrDest;

        format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);
        nEdlen = (short)snprintf(sEdata, DATA_SIZE,
                                 "UNKNOWN_REALM&%s", pstHdr->destination);
        if (sSenderRealm[0]) {
            psErrDest = sSenderRealm;
        } else {
            psErrDest = "";
        }
        build_frame(&stErrFrame, MSG_UNKNOWN_REALM, sOrigin,
                    (char *)psErrDest, sEdata, nEdlen);
        send_frame(nClientFd, &stErrFrame);

        nLen = asprintf(&psOutput,
                        "No route to %s — UNKNOWN_REALM sent to origin.\n",
                        pstHdr->destination);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return;
    }

    nLen = asprintf(&psOutput, "Found route: %s -> %s:%d\nForwarding...\n",
                    pstHdr->destination, sIp, nPort);
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }

    /* Open a second socket to the next hop and bridge both connections. */
    nFd2 = connect_to_realm(sIp, nPort);
    if (nFd2 < 0) {
        nLen = asprintf(&psOutput,
                        "Could not connect to %s for forwarding.\n",
                        pstHdr->destination);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
        return;
    }

    /* The ORIGIN and DESTINATION fields in pstHdr are NOT modified here.
     * Intermediate hops must preserve them so the destination can parse the
     * sender realm name and so the final recipient can send its 0x03 ACCEPT/REJECT
     * directly back to the true origin address encoded in ORIGIN. */
    if (send_frame(nFd2, pstHdr) < 0) {
        close(nFd2);
        return;
    }

    /* Validate every frame received from the next hop before forwarding it.
     * A bad checksum means the network or the next node corrupted the frame;
     * we send NACK so the sender knows to abort rather than hanging. */
    if (0 != recv_frame(nFd2, &stAck)) {
        close(nFd2);
        return;
    }
    if (!validate_frame(&stAck)) {
        /* Send NACK back to the next hop (nFd2) */
        Frame stNackF;
        char  sNackOrigin[ORIGIN_SIZE];
        char  sNackData[DATA_SIZE];
        short nNlen;

        format_origin(sNackOrigin, pstMaester->listen_ip,
                      pstMaester->listen_port);
        nNlen = (short)snprintf(sNackData, DATA_SIZE, "%s",
                                pstMaester->realm_name);
        build_frame(&stNackF, MSG_NACK, sNackOrigin, "", sNackData, nNlen);
        send_frame(nFd2, &stNackF);
        close(nFd2);
        return;
    }
    if (send_frame(nClientFd, &stAck) < 0) {
        close(nFd2);
        return;
    }

    /* KO means the destination rejected the sigil (e.g. already allied or bad
     * path).  No file bytes will follow, so we must not try to relay them. */
    if (stAck.data_length >= 2 &&
        stAck.data[0] == 'K' && stAck.data[1] == 'O') {
        close(nFd2);
        return;
    }

    /* relay_file_frames reads whole 320-byte frames from nClientFd and writes
     * them directly to nFd2 without touching the DATA payload — the intermediate
     * hop never decodes or stores the sigil bytes on disk. */
    if (nFileSize > 0) {
        /* Sigil chunks flow previous-hop -> this Maester -> next-hop. */
        int nRc = relay_file_frames(nClientFd, nFd2, nFileSize, MSG_SIGIL_DATA);
        if (RECV_FRAMES_CKSUM == nRc) {
            /* NACK the previous hop so it stops sending; close forward link. */
            Frame stNackF;
            char  sNackOrigin[ORIGIN_SIZE];
            char  sNackData[DATA_SIZE];
            short nNlen;

            format_origin(sNackOrigin, pstMaester->listen_ip,
                          pstMaester->listen_port);
            nNlen = (short)snprintf(sNackData, DATA_SIZE, "%s",
                                    pstMaester->realm_name);
            build_frame(&stNackF, MSG_NACK, sNackOrigin, "",
                        sNackData, nNlen);
            send_frame(nClientFd, &stNackF);
            close(nFd2);
            return;
        }
        if (RECV_FRAMES_OK != nRc) {
            close(nFd2);
            return;
        }
    }

    /* 4. Relay 0x32 (ACK MD5) from nFd2 back to nClientFd — validate checksum */
    if (0 != recv_frame(nFd2, &stAck)) {
        close(nFd2);
        return;
    }
    if (!validate_frame(&stAck)) {
        Frame stNackF;
        char  sNackOrigin[ORIGIN_SIZE];
        char  sNackData[DATA_SIZE];
        short nNlen;

        format_origin(sNackOrigin, pstMaester->listen_ip,
                      pstMaester->listen_port);
        nNlen = (short)snprintf(sNackData, DATA_SIZE, "%s",
                                pstMaester->realm_name);
        build_frame(&stNackF, MSG_NACK, sNackOrigin, "", sNackData, nNlen);
        send_frame(nFd2, &stNackF);
        close(nFd2);
        return;
    }
    send_frame(nClientFd, &stAck);

    close(nFd2);
}
