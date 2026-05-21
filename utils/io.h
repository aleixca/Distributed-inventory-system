#ifndef __IO_H__
#define __IO_H__

/***********************************************
 *
 * @File:    io.h
 * @Purpose: Declares I/O utility functions and the printF macro for writing to stdout.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/

#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdlib.h>



/* Project-safe stdout write. */
#define printF(x) write(STDOUT_FILENO, x, strlen(x))

/* Uppercase a mutable string. */
void to_upper(char *ps);
/* Strip Windows '\r'. */
void remove_slashr(char *ps);
/* Read fd until separator. */
char *readUntil(int nFd, char cSeparator);

/* Read one stdin line. */
char *read_screen(void);

#endif
