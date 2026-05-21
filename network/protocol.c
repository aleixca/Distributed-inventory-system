/***********************************************
 *
 * @File:    protocol.c
 * @Purpose: Implements frame building, checksum computation, validation, and origin field formatting/parsing.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/********************
 *
 * @Name: compute_checksum
 * @Def: Sum of every byte in the Frame except the two checksum bytes,
 *       taken modulo 65536 (= 2^16) as specified in Annex II.
 * @Arg: In: pstF = pointer to Frame
 * @Ret: 16-bit checksum value
 *
 ********************/
unsigned short compute_checksum(Frame *pstF) {
    unsigned char  buf[FRAME_TOTAL_SIZE];
    unsigned long  nSum = 0;
    unsigned short nDL;
    int            i;

    /* Serialize into a canonical byte layout (little-endian shorts) so the
     * checksum is the same regardless of host byte order. */
    /* Field offsets must match Annex II exactly; never checksum C padding. */
    buf[0] = (unsigned char)pstF->type;
    memcpy(buf + 1,  pstF->origin,      ORIGIN_SIZE);
    memcpy(buf + 21, pstF->destination, DEST_SIZE);
    nDL = (unsigned short)pstF->data_length;
    buf[41] = (unsigned char)(nDL & 0xFF);
    buf[42] = (unsigned char)((nDL >> 8) & 0xFF);
    memcpy(buf + 43, pstF->data, DATA_SIZE);
    /* Checksum field is excluded from the checksum computation. */
    buf[318] = 0;
    buf[319] = 0;

    for (i = 0; i < FRAME_TOTAL_SIZE; i++) {
        nSum += buf[i];
    }
    /* Modulo 65536 (2^16) truncates the accumulated sum to exactly 16 bits,
     * matching the two-byte checksum field defined in Annex II. */
    return (unsigned short)(nSum % 65536UL);
}

/********************
 *
 * @Name: validate_frame
 * @Def: Checks whether the stored checksum matches the recomputed value.
 * @Arg: In: pstF = pointer to Frame
 * @Ret: 1 if valid, 0 if corrupted
 *
 ********************/
int validate_frame(Frame *pstF) {
    /* A frame is valid when the transmitted checksum matches our recompute. */
    return ((unsigned short)pstF->checksum == compute_checksum(pstF));
}

/********************
 *
 * @Name: build_frame
 * @Def: Zeroes the frame, fills all fields, and computes the checksum.
 * @Arg: In: pstF        = Frame to populate
 *       In: cType       = message type byte (MSG_* constant)
 *       In: psOriginStr = "IP:Port" of sending realm (max ORIGIN_SIZE-1 chars)
 *       In: psDestRealm = name of destination realm (max DEST_SIZE-1 chars)
 *       In: psData      = payload bytes (may be NULL for empty)
 *       In: nDataLen    = number of valid bytes in data[]
 * @Ret: None
 *
 ********************/
void build_frame(Frame *pstF, char cType,
                 char *psOriginStr, char *psDestRealm,
                 char *psData, short nDataLen) {
    /* Zeroing first pads unused fields and data bytes with '\0'. */
    memset(pstF, 0, sizeof(Frame));
    pstF->type = cType;

    if (NULL != psOriginStr)
        strncpy(pstF->origin, psOriginStr, ORIGIN_SIZE - 1);

    if (NULL != psDestRealm)
        strncpy(pstF->destination, psDestRealm, DEST_SIZE - 1);

    /* data_length says how many bytes of data[] are meaningful. */
    pstF->data_length = nDataLen;
    if (NULL != psData && nDataLen > 0)
        memcpy(pstF->data, psData, (size_t)nDataLen);

    /* Checksum is computed last because it depends on all previous fields. */
    pstF->checksum = (short)compute_checksum(pstF);
}

/********************
 *
 * @Name: format_origin
 * @Def: Formats "IP:Port" into psBuf for use as the ORIGIN field.
 * @Arg: Out: psBuf = destination buffer (at least ORIGIN_SIZE bytes)
 *       In:  psIp  = IP address string
 *       In:  nPort = port number
 * @Ret: None
 *
 ********************/
void format_origin(char *psBuf, char *psIp, int nPort) {
    /* ORIGIN is always stored as text so it can be parsed across machines. */
    snprintf(psBuf, ORIGIN_SIZE, "%s:%d", psIp, nPort);
    psBuf[ORIGIN_SIZE - 1] = '\0';
}

/********************
 *
 * @Name: parse_origin
 * @Def: Parses an ORIGIN field "IP:Port" into separate psIpOut and pnPortOut.
 * @Arg: In:  psOrigin   = ORIGIN field string (e.g. "192.168.1.3:9003")
 *       Out: psIpOut    = buffer to receive the IP string (at least 16 bytes)
 *       Out: pnPortOut  = receives the port number
 * @Ret: 0 on success, -1 if no colon found
 *
 ********************/
int parse_origin(char *psOrigin, char *psIpOut, int *pnPortOut) {
    char *psColon = NULL;
    int   nIpLen;

    if (NULL == psOrigin || NULL == psIpOut || NULL == pnPortOut) {
        return -1;
    }

    /* The colon separates the dotted IPv4 string from the decimal port. */
    psColon = strchr(psOrigin, ':');
    if (NULL == psColon) {
        return -1;
    }

    nIpLen = (int)(psColon - psOrigin);
    if (nIpLen <= 0 || nIpLen > 15) {
        return -1;
    }

    strncpy(psIpOut, psOrigin, (size_t)nIpLen);
    psIpOut[nIpLen] = '\0';
    *pnPortOut = atoi(psColon + 1);
    return 0;
}
