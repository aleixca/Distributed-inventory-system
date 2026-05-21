#ifndef __RELAY_H__
#define __RELAY_H__

/***********************************************
 *
 * @File:    relay.h
 * @Purpose: Declares the relay_pledge_hop() function for forwarding PLEDGE frames at intermediate nodes.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE

#include "protocol.h"
#include "maester_types.h"

/* Intermediate-hop relay for PLEDGE (0x01) exchanges.
 *
 * Called when we receive a 0x01 frame but are NOT the destination.
 * Performs the full relay sequence on the already-open client_fd:
 *   1. Look up next-hop route to frame->destination
 *   2. Connect to next hop (fd2)
 *   3. Forward 0x01 to fd2
 *   4. Relay 0x31 (ACK FILE) from fd2 back to client_fd
 *   5. Relay 0x02 (sigil data) frames from client_fd to fd2
 *   6. Relay 0x32 (ACK MD5)   from fd2 back to client_fd
 *   7. Close fd2
 *
 * Intermediate hops do NOT modify ORIGIN or DESTINATION fields.
 *
 * Prints hop log on stdout before forwarding.
 */
void relay_pledge_hop(Frame *pstHdr, int nClientFd, Maester *pstMaester);

#endif
