#ifndef __TERMINAL_H__
#define __TERMINAL_H__

/***********************************************
 *
 * @File:    terminal.h
 * @Purpose: Declares the terminal() function and includes all subsystem headers needed by the terminal loop.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/

#include "commands.h"
#include "io.h"
#include <signal.h>
#include "maester_types.h"
#include "inventory.h"
#include "pledge.h"
#include "protocol.h"
#include "network.h"
#include "message_handler.h"
#include "envoy.h"

#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>

void terminal(int *pnTotalProducts, Product **ppstProducts, Maester *pstMaester,
              int nServerFd, volatile sig_atomic_t *pnStopFlag);

#endif
