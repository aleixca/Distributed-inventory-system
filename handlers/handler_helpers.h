#ifndef __HANDLER_HELPERS_H__
#define __HANDLER_HELPERS_H__

/***********************************************
 *
 * @File:    handler_helpers.h
 * @Purpose: Declares internal helper functions and handler prototypes used by the message handler modules.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/

#include "message_handler.h"

/* ── Internal helper prototypes ─────────────────────────────────────────── */

void parse_data_fields(char *psDataCopy, int nDataLen, char **ppsF0, char **ppsF1, char **ppsF2, char **ppsF3);

/* Send 0x31 ACK FILE with OK/KO status. */
void send_ack_file(int nFd, char *psOriginStr, char *psDestRealm, char *psResultStr, char *psOurRealm);

/* Send 0x32 ACK MD5 with CHECK_OK/CHECK_KO status. */
void send_ack_md5(int nFd, char *psOriginStr, char *psDestRealm, char *psResultStr, char *psOurRealm);

/* Resolve an incoming frame's ORIGIN back to a known pledged realm. */
char *find_realm_by_origin(char *psOriginField);

/* Create a directory when needed; success if it already exists. */
int ensure_dir(const char *psPath);

/* Receive one frame and verify checksum, sending NACK on checksum failure. */
int recv_validated(int nFd, Frame *pstF, Maester *pstM);

/* Send NACK over an already-open socket. */
void send_nack_on_fd(int nFd, Maester *pstM);

/* ── Handler prototypes ──────────────────────────────────────────────────── */

void handle_alliance_dest(Frame *pstFrame, int nClientFd, Maester *pstMaester);
void handle_alliance_resp(Frame *pstFrame, int nClientFd, Maester *pstMaester);
void handle_list_request(Frame *pstFrame, int nClientFd, Maester *pstMaester, Product **ppstProducts, int *pnTotalProducts);
void handle_order_header(Frame *pstFrame, int nClientFd, Maester *pstMaester, Product **ppstProducts, int *pnTotalProducts);
void handle_disconnect(Frame *pstFrame);

#endif
