#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

/***********************************************
 *
 * @File:    protocol.h
 * @Purpose: Defines the fixed 320-byte Frame structure, message type constants, and protocol helper function declarations.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE

/* ── Frame dimensions (Annex II: fixed 320-byte frame) ─────────────────── */
#define FRAME_TOTAL_SIZE  320
/* "IP:Port", e.g. "192.168.1.3:9003". */
#define ORIGIN_SIZE        20
/* Realm name, e.g. "TheVale". */
#define DEST_SIZE          20
/* 320 - 1 - 20 - 20 - 2 - 2 = 275. */
#define DATA_SIZE         275

/* ── Message type bytes (Annex II) ─────────────────────────────────────── */
/* A to B: alliance request header. */
#define MSG_ALLIANCE_HEADER  '\x01'
/* A to B: sigil file data chunk. */
#define MSG_SIGIL_DATA       '\x02'
/* B to A: ACCEPT or REJECT. */
#define MSG_ALLIANCE_RESP    '\x03'
/* A to B: product list request. */
#define MSG_LIST_REQUEST     '\x11'
/* B to A: product list header. */
#define MSG_LIST_HEADER      '\x12'
/* B to A: product list file data. */
#define MSG_LIST_DATA        '\x13'
/* A to B: order header. */
#define MSG_ORDER_HEADER     '\x14'
/* A to B: order file data. */
#define MSG_ORDER_DATA       '\x15'
/* B to A: OK or REJECT. */
#define MSG_ORDER_RESP       '\x16'
/* Hop to origin: no route to destination. */
#define MSG_UNKNOWN_REALM    '\x21'
/* B to A: no alliance. */
#define MSG_UNAUTHORIZED     '\x25'
/* A to ally: shutting down. */
#define MSG_DISCONNECT       '\x27'
/* Receiver to sender: ready to receive file. */
#define MSG_ACK_FILE         '\x31'
/* Receiver to sender: MD5 result. */
#define MSG_ACK_MD5          '\x32'
/* Any realm: bad checksum detected. */
#define MSG_NACK             '\x69'

/* ── DATA field separators (Annex II uses '&') ─────────────────────────── */
#define DATA_SEP  '&'

/*
 * Frame layout (must be exactly 320 bytes, no padding):
 *
 *  Offset  Size  Field
 *  ------  ----  -----
 *       0     1  type
 *       1    20  origin      "IP:Port" of sending realm
 *      21    20  destination realm name of target
 *      41     2  data_length valid bytes in data[]
 *      43   275  data        payload (padded with '\0')
 *     318     2  checksum    sum of all other bytes % 65536
 */
typedef struct {
    char           type;
    char           origin[ORIGIN_SIZE];
    char           destination[DEST_SIZE];
    short          data_length;
    char           data[DATA_SIZE];
    short          checksum;
} Frame;

/* ── Protocol functions ─────────────────────────────────────────────────── */

/* Compute checksum: sum of all bytes in frame except checksum bytes, % 65536 */
unsigned short compute_checksum(Frame *pstF);

/* Return 1 if frame->checksum is correct, 0 otherwise */
int validate_frame(Frame *pstF);

/*
 * Zero the frame, fill all fields, compute and set checksum.
 * psOriginStr : "IP:Port" string (at most ORIGIN_SIZE-1 chars)
 * psDestRealm : realm name (at most DEST_SIZE-1 chars)
 * psData      : payload bytes (NULL → zero data)
 * nDataLen    : number of valid bytes in data (0..DATA_SIZE)
 */
void build_frame(Frame *pstF, char cType, char *psOriginStr, char *psDestRealm, char *psData, short nDataLen);

/* Format "IP:Port" into psBuf (psBuf must be at least ORIGIN_SIZE bytes) */
void format_origin(char *psBuf, char *psIp, int nPort);

/* Parse "IP:Port" string → separate psIpOut and *pnPortOut.
 * psIpOut must hold at least 16 bytes.
 * Returns 0 on success, -1 if no colon found. */
int parse_origin(char *psOrigin, char *psIpOut, int *pnPortOut);

#endif
