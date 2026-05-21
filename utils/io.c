/***********************************************
 *
 * @File:    io.c
 * @Purpose: Provides low-level I/O helpers: string uppercasing, reading from file descriptors, and stdin line reading.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#include "io.h"


/********************
 *
 * @Name: to_upper
 * @Def: Converts a string to uppercase.
 * @Arg: In: ps = pointer to the string to convert
 * @Ret: None
 *
 ********************/
void to_upper(char *ps) {
    /* Used for command keywords; callers must avoid uppercasing file paths. */
    for (int i = 0; ps[i] != '\0'; i++) {
        ps[i] = (char)toupper((char)ps[i]);
    }
}


/********************
 *
 * @Name: remove_slashr
 * @Def: Removes the carriage return character from a string.
 * @Arg: In: ps = pointer to the string to modify
 * @Ret: None
 *
 ********************/
void remove_slashr(char *ps) {
    int i = 0;
    /* Windows text files may include '\r\n'; config parsing wants plain '\n'. */
    while (ps[i] != '\0') {
        if (ps[i] == '\r') {
            ps[i] = '\0';
            return;
        }
        i++;
    }
}

/********************
 *
 * @Name: readUntil
 * @Def: Reads characters from a file descriptor until a specified separator is encountered.
 * @Arg: In: nFd        = file descriptor to read from
 *       In: cSeparator = character that indicates the end of reading
 * @Ret: Pointer to the read string, or NULL when it fails
 *
 ********************/
char *readUntil(int nFd, char cSeparator) {
    char  cCh;
    int   i = 0;
    int   nCapacity = 16;
    char *psBuffer;
    char *psNewBuf;

    psBuffer = malloc(nCapacity);
    if (NULL == psBuffer) {
        return NULL;
    }

    /* Read byte by byte because config fields use different separators. */
    while (1 == read(nFd, &cCh, 1)) {
        if (cCh == cSeparator) {
            psBuffer[i] = '\0';
            return psBuffer;
        }

        /* Grow buffer dynamically so config fields are not fixed-length. */
        if (i >= nCapacity - 1) {
            nCapacity *= 2;
            psNewBuf = realloc(psBuffer, nCapacity);
            if (NULL == psNewBuf) {
                free(psBuffer);
                return NULL;
            }
            psBuffer = psNewBuf;
        }

        psBuffer[i++] = cCh;
    }

    /* EOF or error */
    if (0 == i) {
        free(psBuffer);
        return NULL;
    }

    psBuffer[i] = '\0';
    return psBuffer;
}

/********************
 *
 * @Name: read_screen
 * @Def: Reads a line of input from the user.
 * @Arg: None
 * @Ret: Pointer to the read string, or NULL when it fails.
 *
 ********************/
char *read_screen(void) {
    char cC;
    int nNumOfChars = 0;
    int nBytes;
    char *psWord;
    char *psTmp;

    psWord = malloc(1);
    if (NULL == psWord) {
        return NULL;
    }

    /* read() on fd 0 is used instead of fgets/scanf because select() in
     * terminal.c monitors STDIN_FILENO directly.  Mixing stdio buffering
     * with select() can cause select() to report the fd as not ready even
     * though characters are waiting in the stdio buffer, leading to a
     * stalled prompt.  Using raw read() keeps all I/O at the fd level. */
    while (1) {
        nBytes = read(0, &cC, 1);

        if (nBytes <= 0) {
            free(psWord);
            return NULL;
        }

        if (cC == '\n') {
            break;
        }

        /* Grow by one character at a time.  nNumOfChars + 2 ensures there
         * is always room for the current character and the final '\0'. */
        psTmp = realloc(psWord, nNumOfChars + 2);
        if (NULL == psTmp) {
            free(psWord);
            return NULL;
        }

        psWord = psTmp;
        psWord[nNumOfChars] = cC;
        nNumOfChars++;
    }

    psWord[nNumOfChars] = '\0';
    return psWord;
}
