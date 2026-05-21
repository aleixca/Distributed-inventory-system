#ifndef __SIGIL_H__
#define __SIGIL_H__

/***********************************************
 *
 * @File:    sigil.h
 * @Purpose: Declares file MD5/size utilities and frame-based file transfer functions for the alliance protocol.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE

#include <unistd.h>
#include "protocol.h"

/* ── MD5 and file size ───────────────────────────────────────────────────── */

/* Compute MD5 of a local file using fork + execvp("md5sum").
 * Returns malloc'd 32-char hex string. Caller must free(). NULL on error. */
char *compute_file_md5(char *psFilepath);

/* Return size of psFilepath in bytes, or -1 on error. */
int get_file_size(char *psFilepath);

/* ── Frame-based file transfer (Annex II protocol) ──────────────────────── */

/* Send the entire file at psFilepath as successive MSG_* frames on nFd.
 * Each frame carries up to DATA_SIZE bytes of file content.
 * psOriginStr : ORIGIN field ("IP:Port") for each built frame
 * psDestRealm : DESTINATION field for each built frame
 * Returns 0 on success, -1 on error. */
int send_file_in_frames(int nFd, char *psFilepath, char cType, char *psOriginStr, char *psDestRealm);

/* Return codes for recv_file_in_frames and relay_file_frames */
#define RECV_FRAMES_OK     0
/* I/O error, wrong frame type, or short read. */
#define RECV_FRAMES_ERR   -1
/* Invalid checksum; caller must send NACK. */
#define RECV_FRAMES_CKSUM -2

/* Receive exactly nExpectedSize bytes from successive frames of cExpectedType
 * on nFd and write them to psSavePath.
 * Returns RECV_FRAMES_OK, RECV_FRAMES_ERR, or RECV_FRAMES_CKSUM. */
int recv_file_in_frames(int nFd, char *psSavePath, int nExpectedSize, char cExpectedType);

/* Relay file frames between two open connections without touching disk.
 * Reads nExpectedSize bytes worth of cExpectedType frames from nSrcFd and
 * writes each frame unchanged to nDstFd.
 * Returns 0 on success, -1 on error. */
int relay_file_frames(int nSrcFd, int nDstFd, int nExpectedSize, char cExpectedType);

#endif
