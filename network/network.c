/***********************************************
 *
 * @File:    network.c
 * @Purpose: Implements TCP socket creation, connection, frame sending/receiving, and realm routing.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "network.h"
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

/********************
 *
 * @Name: create_server_socket
 * @Def: Creates, binds, and starts listening on a TCP server socket.
 * @Arg: In: psIp  = IP address string to bind to
 *       In: nPort = port number to listen on
 * @Ret: File descriptor on success, -1 on failure
 *
 ********************/
int create_server_socket(char *psIp, int nPort) {
    int                nFd;
    int                nOpt = 1;
    struct sockaddr_in stAddr;

    /* TCP/IPv4 socket used by the Maester to receive protocol frames. */
    nFd = socket(AF_INET, SOCK_STREAM, 0);
    if (nFd < 0) {
        return -1;
    }

    /* Allows restarting the Maester quickly without waiting for TIME_WAIT. */
    setsockopt(nFd, SOL_SOCKET, SO_REUSEADDR, &nOpt, sizeof(nOpt));

    memset(&stAddr, 0, sizeof(stAddr));
    stAddr.sin_family = AF_INET;
    stAddr.sin_port   = htons((short)nPort);
    if (inet_pton(AF_INET, psIp, &stAddr.sin_addr) <= 0) {
        close(nFd);
        return -1;
    }
    if (bind(nFd, (struct sockaddr *)&stAddr, sizeof(stAddr)) < 0) {
        close(nFd);
        return -1;
    }
    if (listen(nFd, SOMAXCONN) < 0) {
        close(nFd);
        return -1;
    }

    return nFd;
}

/********************
 *
 * @Name: connect_to_realm
 * @Def: Opens a TCP connection to the given IP and port.
 * @Arg: In: psIp  = destination IP address string
 *       In: nPort = destination port number
 * @Ret: Connected file descriptor on success, -1 on failure
 *
 ********************/
int connect_to_realm(char *psIp, int nPort) {
    int                nFd;
    struct sockaddr_in stAddr;
    int                nFlags;
    int                nRc;
    int                nSavedErrno;
    fd_set             stWfds;
    struct timeval     stTv;
    int                nConnErr;
    socklen_t          nErrLen;
    char              *psOutput;
    int                nLen;

    /* Outgoing TCP socket used by Envoys and relay hops. */
    nFd = socket(AF_INET, SOCK_STREAM, 0);
    if (nFd < 0) {
        return -1;
    }

    memset(&stAddr, 0, sizeof(stAddr));
    stAddr.sin_family = AF_INET;
    stAddr.sin_port   = htons((short)nPort);
    if (inet_pton(AF_INET, psIp, &stAddr.sin_addr) <= 0) {
        close(nFd);
        return -1;
    }

    /* Non-blocking connect with 10-second timeout. */
    /* Temporarily switch to non-blocking mode so connect() can time out. */
    nFlags = fcntl(nFd, F_GETFL, 0);
    fcntl(nFd, F_SETFL, nFlags | O_NONBLOCK);

    nRc = connect(nFd, (struct sockaddr *)&stAddr, sizeof(stAddr));
    if (nRc < 0) {
        nSavedErrno = errno;
        if (EINPROGRESS != nSavedErrno) {
            /* Human-readable diagnostics for the most common cross-machine
             * setup failures.  These messages map directly to the "Known
             * Outstanding Issue" scenarios: wrong IP in config, firewall
             * blocking the port, or the remote Maester not yet started. */
            if (ECONNREFUSED == nSavedErrno) {
                nLen = asprintf(&psOutput,
                    "[NET] connect %s:%d — Connection refused "
                    "(is the remote Maester running?)\n", psIp, nPort);
            } else if (ENETUNREACH == nSavedErrno || EHOSTUNREACH == nSavedErrno) {
                nLen = asprintf(&psOutput,
                    "[NET] connect %s:%d — Network unreachable "
                    "(check IP addresses and LAN connectivity / ping)\n", psIp, nPort);
            } else {
                nLen = asprintf(&psOutput,
                    "[NET] connect %s:%d — errno %d (%s)\n",
                    psIp, nPort, nSavedErrno, strerror(nSavedErrno));
            }
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
            close(nFd);
            return -1;
        }

        /* select() waits until the socket becomes writable or timeout expires. */
        FD_ZERO(&stWfds);
        FD_SET(nFd, &stWfds);
        stTv.tv_sec  = 10;
        stTv.tv_usec = 0;
        if (select(nFd + 1, NULL, &stWfds, NULL, &stTv) <= 0) {
            nLen = asprintf(&psOutput,
                "[NET] connect %s:%d — timed out after 10 s "
                "(firewall dropping packets? host unreachable?)\n", psIp, nPort);
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
            close(nFd);
            return -1;
        }

        /* SO_ERROR tells whether the async connect actually succeeded. */
        nConnErr = 0;
        nErrLen  = (socklen_t)sizeof(nConnErr);
        if (getsockopt(nFd, SOL_SOCKET, SO_ERROR, &nConnErr, &nErrLen) < 0 ||
            0 != nConnErr) {
            nLen = asprintf(&psOutput,
                "[NET] connect %s:%d — async connect failed: %s\n",
                psIp, nPort, strerror(nConnErr));
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
            close(nFd);
            return -1;
        }
    }

    /* Restore original flags after the connection has been established. */
    fcntl(nFd, F_SETFL, nFlags);
    set_socket_timeout(nFd, 30);
    return nFd;
}

/********************
 *
 * @Name: set_socket_timeout
 * @Def: Sets SO_RCVTIMEO and SO_SNDTIMEO on nFd to nTimeoutSec seconds.
 * @Arg: In: nFd        = socket file descriptor
 *       In: nTimeoutSec = timeout in seconds
 * @Ret: None
 *
 ********************/
void set_socket_timeout(int nFd, int nTimeoutSec) {
    struct timeval stTv;

    /* Both reads and writes fail instead of blocking forever. */
    stTv.tv_sec  = nTimeoutSec;
    stTv.tv_usec = 0;
    setsockopt(nFd, SOL_SOCKET, SO_RCVTIMEO, &stTv, sizeof(stTv));
    setsockopt(nFd, SOL_SOCKET, SO_SNDTIMEO, &stTv, sizeof(stTv));
}

/********************
 *
 * @Name: lookup_route
 * @Def: Resolves the IP and port for a destination realm.
 *       Priority: (1) direct ally route, (2) named route, (3) DEFAULT route.
 * @Arg: In:  psRealm    = destination realm name
 *       In:  pstMaester = local Maester config with route table
 *       Out: psIpOut    = buffer of at least IP_SIZE (16) bytes to receive IP
 *       Out: pnPortOut  = receives the port number
 * @Ret: 0 on success, -1 if no route found
 *
 ********************/
int lookup_route(char *psRealm, Maester *pstMaester, char *psIpOut, int *pnPortOut) {
    int  nDefaultIdx = -1;
    char sTmpIp[IP_SIZE];
    int  nTmpPort;
    int  i;

    /* Direct route: confirmed allies communicate without hops */
    /* Phase 3/4 rule: allied realms use their stored direct address. */
    if (PLEDGE_ALLIED == get_pledge_status(psRealm)) {
        if (0 == get_pledge_ip_port(psRealm, sTmpIp, &nTmpPort)) {
            strncpy(psIpOut, sTmpIp, IP_SIZE - 1);
            psIpOut[IP_SIZE - 1] = '\0';
            *pnPortOut = nTmpPort;
            return 0;
        }
    }

    /* Named route in routing table */
    /* Otherwise search static routes from the configuration file. */
    for (i = 0; i < pstMaester->route_count; i++) {
        if (0 == strcasecmp(pstMaester->routes[i].maester, "DEFAULT")) {
            nDefaultIdx = i;
            continue;
        }
        if (0 == strcasecmp(pstMaester->routes[i].maester, psRealm)) {
            /* wildcard route: realm exists but address unknown — use DEFAULT */
            if (0 == pstMaester->routes[i].port ||
                pstMaester->routes[i].ip[0] == '*')
                continue;
            strncpy(psIpOut, pstMaester->routes[i].ip, IP_SIZE - 1);
            psIpOut[IP_SIZE - 1] = '\0';
            *pnPortOut = pstMaester->routes[i].port;
            return 0;
        }
    }

    /* DEFAULT fallback */
    /* DEFAULT is the final fallback for multi-hop delivery. */
    if (nDefaultIdx >= 0) {
        strncpy(psIpOut, pstMaester->routes[nDefaultIdx].ip, IP_SIZE - 1);
        psIpOut[IP_SIZE - 1] = '\0';
        *pnPortOut = pstMaester->routes[nDefaultIdx].port;
        return 0;
    }

    return -1;
}

/********************
 *
 * @Name: send_frame
 * @Def: Writes exactly sizeof(Frame) = 320 bytes to nFd in a loop.
 * @Arg: In: nFd      = file descriptor to write to
 *       In: pstFrame = pointer to the Frame to send
 * @Ret: Total bytes written on success, -1 on error
 *
 ********************/
int send_frame(int nFd, Frame *pstFrame) {
    unsigned char  buf[FRAME_TOTAL_SIZE];
    unsigned short nDL;
    unsigned short nCS;
    int            nTotal     = 0;
    int            nRemaining = FRAME_TOTAL_SIZE;
    int            nWritten;

    /* Serialize manually into exactly 320 bytes before writing to the socket.
     * Writing the struct directly would include C compiler padding and make
     * the layout host-dependent; two machines with different ABIs would then
     * misread each other's frames.  The manual byte-by-byte layout guarantees
     * the wire format matches Annex II on every platform. */
    buf[0] = (unsigned char)pstFrame->type;
    memcpy(buf + 1,  pstFrame->origin,      ORIGIN_SIZE);
    memcpy(buf + 21, pstFrame->destination, DEST_SIZE);
    nDL = (unsigned short)pstFrame->data_length;
    buf[41] = (unsigned char)(nDL & 0xFF);
    buf[42] = (unsigned char)((nDL >> 8) & 0xFF);
    memcpy(buf + 43, pstFrame->data, DATA_SIZE);
    nCS = (unsigned short)pstFrame->checksum;
    buf[318] = (unsigned char)(nCS & 0xFF);
    buf[319] = (unsigned char)((nCS >> 8) & 0xFF);

    /* write() may send fewer bytes than requested, so loop until all 320 go out. */
    while (nRemaining > 0) {
        nWritten = (int)write(nFd, (char *)buf + nTotal, (size_t)nRemaining);
        if (nWritten <= 0) {
            return -1;
        }
        nTotal     += nWritten;
        nRemaining -= nWritten;
    }
    return nTotal;
}

/********************
 *
 * @Name: recv_frame
 * @Def: Reads exactly sizeof(Frame) = 320 bytes from nFd in a loop.
 * @Arg: In:  nFd      = file descriptor to read from
 *       Out: pstFrame = buffer to fill
 * @Ret: 0 on success, -1 on EOF or error
 *
 ********************/
int recv_frame(int nFd, Frame *pstFrame) {
    unsigned char buf[FRAME_TOTAL_SIZE];
    int           nTotal     = 0;
    int           nRemaining = FRAME_TOTAL_SIZE;
    int           nRead;

    /* read() may return a partial frame, so keep reading until 320 bytes arrive. */
    while (nRemaining > 0) {
        nRead = (int)read(nFd, (char *)buf + nTotal, (size_t)nRemaining);
        if (nRead <= 0) {
            return -1;
        }
        nTotal     += nRead;
        nRemaining -= nRead;
    }

    /* Decode the canonical byte layout back into the local Frame structure. */
    pstFrame->type = (char)buf[0];
    memcpy(pstFrame->origin,      buf + 1,  ORIGIN_SIZE);
    memcpy(pstFrame->destination, buf + 21, DEST_SIZE);
    pstFrame->data_length = (short)((unsigned short)buf[41] |
                                    ((unsigned short)buf[42] << 8));
    memcpy(pstFrame->data, buf + 43, DATA_SIZE);
    pstFrame->checksum    = (short)((unsigned short)buf[318] |
                                    ((unsigned short)buf[319] << 8));
    return 0;
}

/********************
 *
 * @Name: send_nack
 * @Def: Connects to psDestIp:nDestPort and sends a NACK (0x69) frame.
 *       DATA = our realm name, as specified in Annex II.
 * @Arg: In: psDestIp   = IP of the recipient
 *       In: nDestPort  = port of the recipient
 *       In: pstMaester = local Maester (provides ORIGIN and realm name)
 * @Ret: None
 *
 ********************/
void send_nack(char *psDestIp, int nDestPort, Maester *pstMaester) {
    Frame stFrame;
    char  sOrigin[ORIGIN_SIZE];
    char  sData[DATA_SIZE];
    short nDataLen;
    int   nFd;

    /* NACK opens a fresh connection when the original one is not being reused. */
    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);
    nDataLen = (short)snprintf(sData, DATA_SIZE, "%s", pstMaester->realm_name);
    build_frame(&stFrame, MSG_NACK, sOrigin, "", sData, nDataLen);

    nFd = connect_to_realm(psDestIp, nDestPort);
    if (nFd < 0) {
        return;
    }
    send_frame(nFd, &stFrame);
    close(nFd);
}

/********************
 *
 * @Name: broadcast_shutdown
 * @Def: Sends a DISCONNECT (0x27) frame to every currently allied realm.
 *       DATA = "DISCONNECT" as specified in Annex II.
 * @Arg: In: pstMaester = local Maester (provides realm name and address)
 * @Ret: None
 *
 ********************/
void broadcast_shutdown(Maester *pstMaester) {
    int   nCount = get_pledge_count();
    char *psOutput;
    int   nLen;
    int   i;
    char  sOrigin[ORIGIN_SIZE];

    format_origin(sOrigin, pstMaester->listen_ip, pstMaester->listen_port);

    /* Notify every active ally so it marks this realm as INACTIVE. */
    for (i = 0; i < nCount; i++) {
        char  *psRealm = get_pledge_realm(i);
        char   sIp[IP_SIZE];
        int    nPort;
        int    nFd;
        Frame  stFrame;
        char   sData[] = "DISCONNECT";

        if (NULL == psRealm) {
            continue;
        }
        if (PLEDGE_ALLIED != get_pledge_status(psRealm)) {
            continue;
        }
        if (0 != get_pledge_ip_port(psRealm, sIp, &nPort)) {
            continue;
        }

        build_frame(&stFrame, MSG_DISCONNECT, sOrigin, psRealm,
                    sData, (short)strlen(sData));

        nFd = connect_to_realm(sIp, nPort);
        if (nFd < 0) {
            continue;
        }
        send_frame(nFd, &stFrame);
        close(nFd);

        nLen = asprintf(&psOutput, "Shutdown notice sent to %s.\n", psRealm);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
    }
}
