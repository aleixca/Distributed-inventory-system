#ifndef __PIPE_MSG_H__
#define __PIPE_MSG_H__

/***********************************************
 *
 * @File:    pipe_msg.h
 * @Purpose: Defines the pipe message protocol (PipeMsgHeader, PipeMsgType) used for envoy child-to-parent communication.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE

#include "inventory.h"
#include <unistd.h>

/* ── Pipe message protocol (child → parent) ───────────────────────────────
 * Each envoy child writes one PipeMsgHeader to its result pipe, optionally
 * followed by `count` Product structs (for LIST_OK and TRADE_OK).
 * The parent reads the header first, then the payload if count > 0.
 * ─────────────────────────────────────────────────────────────────────── */

typedef enum {
    PIPE_PLEDGE_OK,
    PIPE_PLEDGE_FAIL,
    PIPE_LIST_OK,
    PIPE_LIST_FAIL,
    PIPE_TRADE_OK,
    PIPE_TRADE_FAIL,
    /* Pledge response sent; count stores the new pledge status. */
    PIPE_RESPOND_DONE
} PipeMsgType;

typedef struct {
    /* PIPE_* result type. */
    PipeMsgType type;
    /* Realm associated with the completed mission. */
    char        realm[50];
    /* Product entries that follow (LIST_OK / TRADE_OK). */
    int         count;
} PipeMsgHeader;

static inline int write_all(int nFd, const void *pBuf, size_t nN) {
    size_t  nDone = 0;
    ssize_t nW;

    /* Pipes can also perform partial writes, so loop until complete. */
    while (nDone < nN) {
        nW = write(nFd, (const char *)pBuf + nDone, nN - nDone);
        if (nW <= 0) return -1;
        nDone += (size_t)nW;
    }
    return 0;
}

static inline int read_all(int nFd, void *pBuf, size_t nN) {
    size_t  nDone = 0;
    ssize_t nR;

    /* Parent/child pipe messages must be read in full fixed-size blocks. */
    while (nDone < nN) {
        nR = read(nFd, (char *)pBuf + nDone, nN - nDone);
        if (nR <= 0) return -1;
        nDone += (size_t)nR;
    }
    return 0;
}

#endif
