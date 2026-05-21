/***********************************************
 *
 * @File:    sigil.c
 * @Purpose: Implements file MD5 computation, size querying, and frame-based file send/receive/relay operations.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "sigil.h"
#include "network.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

/********************
 *
 * @Name: compute_file_md5
 * @Def: Forks a child that execs md5sum on psFilepath, reads the 32-char hex
 *       hash from the pipe, and returns it as a malloc'd string.
 *       Uses fork+pipe+execvp+waitpid per session S3 constraints.
 * @Arg: In: psFilepath = path to the file to hash
 * @Ret: malloc'd 32-char hex string, or NULL on error. Caller must free.
 *
 ********************/
char *compute_file_md5(char *psFilepath) {
    int nPipefd[2];
    int nPid;
    char *psResult;
    char *psArgs[3];
    int nN;
    int nR;

    /* Pipe carries stdout from the md5sum child back to the parent. */
    if (-1 == pipe(nPipefd)) {
        return NULL;
    }

    nPid = fork();
    if (-1 == nPid) {
        close(nPipefd[0]);
        close(nPipefd[1]);
        return NULL;
    }

    if (0 == nPid) {
        /* dup2 replaces file-descriptor 1 (stdout) with the pipe write-end so
         * that md5sum's normal output goes into the pipe instead of the terminal.
         * The original write-end fd is closed afterwards to avoid a duplicate. */
        close(nPipefd[0]);
        if (-1 == dup2(nPipefd[1], STDOUT_FILENO)) {
            _exit(1);
        }
        close(nPipefd[1]);
        psArgs[0] = "md5sum";
        psArgs[1] = psFilepath;
        psArgs[2] = NULL;
        /* execvp searches PATH so "md5sum" resolves on any Linux distro without
         * hard-coding /usr/bin/md5sum. */
        execvp("md5sum", psArgs);
        /* _exit instead of exit: avoids flushing stdio buffers that belong to
         * the parent's address space copy, which could corrupt the parent's
         * open file handles or run atexit handlers twice. */
        _exit(1);
    }

    /* Parent closes the write end first: if it kept it open, the pipe would
     * never reach EOF from the child's side and the read loop below would block
     * forever waiting for more data even after md5sum has exited. */
    close(nPipefd[1]);
    psResult = malloc(33);
    if (NULL == psResult) {
        close(nPipefd[0]);
        waitpid(nPid, NULL, 0);
        return NULL;
    }
    nN = 0;
    /* md5sum outputs "HASH  FILENAME\n".  We only want the first 32 hex digits
     * (the hash itself).  read() may return fewer bytes than requested on each
     * call (e.g. if the pipe buffer fills in pieces), so we loop until exactly
     * 32 bytes have been accumulated. */
    while (nN < 32) {
        nR = (int)read(nPipefd[0], psResult + nN, (size_t)(32 - nN));
        if (nR <= 0) {
            break;
        }
        nN += nR;
    }
    psResult[32] = '\0';
    close(nPipefd[0]);
    /* waitpid reaps the child so it does not become a zombie.  We wait after
     * reading so we don't block here before draining the pipe — draining first
     * prevents deadlock when the pipe buffer is smaller than the output. */
    waitpid(nPid, NULL, 0);
    if (nN < 32) {
        free(psResult);
        return NULL;
    }
    return psResult;
}

/********************
 *
 * @Name: get_file_size
 * @Def: Opens a file, seeks to end to measure its size, then closes it.
 * @Arg: In: psFilepath = path to the file
 * @Ret: File size in bytes, or -1 on error.
 *
 ********************/
int get_file_size(char *psFilepath) {
    int nFd;
    int nSize;

    /* lseek avoids stat(), which the statement forbids. */
    nFd = open(psFilepath, O_RDONLY);
    if (-1 == nFd) {
        return -1;
    }
    nSize = (int)lseek(nFd, 0, SEEK_END);
    close(nFd);
    return nSize;
}

/********************
 *
 * @Name: send_file_in_frames
 * @Def: Opens psFilepath and sends its contents as successive protocol frames
 *       of the given type.  Each frame carries up to DATA_SIZE bytes.
 *       Uses build_frame/send_frame so every frame is exactly 320 bytes.
 * @Arg: In: nFd         = connected socket to write to
 *       In: psFilepath  = source file path
 *       In: cType       = MSG_SIGIL_DATA / MSG_LIST_DATA / MSG_ORDER_DATA
 *       In: psOriginStr = ORIGIN field ("IP:Port")
 *       In: psDestRealm = DESTINATION field (realm name)
 * @Ret: 0 on success, -1 on error.
 *
 ********************/
int send_file_in_frames(int nFd, char *psFilepath, char cType, char *psOriginStr, char *psDestRealm) {
    int nFileFd;
    char sChunk[DATA_SIZE];
    int nN;
    Frame stFrame;

    nFileFd = open(psFilepath, O_RDONLY);
    if (-1 == nFileFd) {
        return -1;
    }

    /* Each read becomes one DATA frame, up to the protocol payload size. */
    while ((nN = (int)read(nFileFd, sChunk, DATA_SIZE)) > 0) {
        build_frame(&stFrame, cType, psOriginStr, psDestRealm, sChunk, (short)nN);
        if (send_frame(nFd, &stFrame) < 0) {
            close(nFileFd);
            return -1;
        }
    }
    close(nFileFd);
    if (nN < 0) {
        return -1;
    }
    return 0;
}

/********************
 *
 * @Name: recv_file_in_frames
 * @Def: Receives successive frames of cExpectedType from nFd and writes their
 *       DATA bytes to psSavePath until nExpectedSize bytes are saved.
 * @Arg: In:  nFd           = connected socket to read from
 *       In:  psSavePath    = path to write the reconstructed file
 *       In:  nExpectedSize = total file bytes expected
 *       In:  cExpectedType = MSG_SIGIL_DATA / MSG_LIST_DATA / MSG_ORDER_DATA
 * @Ret: RECV_FRAMES_OK, RECV_FRAMES_ERR, or RECV_FRAMES_CKSUM.
 *
 ********************/
int recv_file_in_frames(int nFd, char *psSavePath, int nExpectedSize, char cExpectedType) {
    int nSaveFd;
    int nReceived = 0;
    Frame stFrame;

    nSaveFd = open(psSavePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (-1 == nSaveFd) {
        return RECV_FRAMES_ERR;
    }

    /* Keep receiving chunks until the announced byte count has been saved. */
    while (nReceived < nExpectedSize) {
        if (0 != recv_frame(nFd, &stFrame)) {
            close(nSaveFd);
            return RECV_FRAMES_ERR;
        }
        /* Any corrupted chunk makes the caller send NACK/CHECK_KO. */
        if (!validate_frame(&stFrame)) {
            close(nSaveFd);
            return RECV_FRAMES_CKSUM;
        }
        if (stFrame.type != cExpectedType) {
            close(nSaveFd);
            return RECV_FRAMES_ERR;
        }
        if (stFrame.data_length <= 0) {
            continue;
        }
        {
            int nW       = 0;
            int nN       = (int)stFrame.data_length;
            int nWritten;

            /* write() can be partial, so loop until this chunk is fully saved. */
            while (nW < nN) {
                nWritten = (int)write(nSaveFd, stFrame.data + nW,
                                      (size_t)(nN - nW));
                if (nWritten <= 0) {
                    close(nSaveFd);
                    return RECV_FRAMES_ERR;
                }
                nW += nWritten;
            }
            nReceived += nN;
        }
    }
    close(nSaveFd);
    return RECV_FRAMES_OK;
}

/********************
 *
 * @Name: relay_file_frames
 * @Def: Relays data frames byte-for-byte from nSrcFd to nDstFd.
 *       Used by intermediate hops that must forward file chunks without
 *       writing to disk.  Each 320-byte frame is read whole then written whole.
 * @Arg: In: nSrcFd        = incoming connection (data source)
 *       In: nDstFd        = outgoing connection (data sink)
 *       In: nExpectedSize = total file bytes being relayed
 *       In: cExpectedType = expected frame type (for safety check)
 * @Ret: 0 on success, -1 on error.
 *
 ********************/
int relay_file_frames(int nSrcFd, int nDstFd, int nExpectedSize,
                       char cExpectedType) {
    int   nRelayed = 0;
    Frame stFrame;

    /* Intermediate hops do not store the file; they validate and forward. */
    while (nRelayed < nExpectedSize) {
        if (0 != recv_frame(nSrcFd, &stFrame)) {
            return RECV_FRAMES_ERR;
        }
        if (!validate_frame(&stFrame)) {
            return RECV_FRAMES_CKSUM;
        }
        if (stFrame.type != cExpectedType) {
            return RECV_FRAMES_ERR;
        }
        if (send_frame(nDstFd, &stFrame) < 0) {
            return RECV_FRAMES_ERR;
        }
        if (stFrame.data_length > 0) {
            nRelayed += (int)stFrame.data_length;
        }
    }
    return RECV_FRAMES_OK;
}
