#ifndef __NETWORK_H__
#define __NETWORK_H__

/***********************************************
 *
 * @File:    network.h
 * @Purpose: Declares socket creation, connection, frame I/O, and broadcast functions for inter-realm communication.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/

#include "protocol.h"
#include "maester_types.h"
#include "pledge.h"

/* ip_out buffers passed to lookup_route / get_pledge_ip_port must be
 * at least IP_SIZE (16) bytes (defined in pledge.h). */

/* Bind/listen local Maester. */
int  create_server_socket(char *psIp, int nPort);
/* Open outgoing TCP connection. */
int  connect_to_realm(char *psIp, int nPort);
/* Resolve direct/named/default route. */
int  lookup_route(char *psRealm, Maester *pstMaester, char *psIpOut, int *pnPortOut);
/* Serialize and send 320 bytes. */
int  send_frame(int nFd, Frame *pstFrame);
/* Read and decode 320 bytes. */
int  recv_frame(int nFd, Frame *pstFrame);
/* Send standalone NACK. */
void send_nack(char *psDestIp, int nDestPort, Maester *pstMaester);
/* Send 0x27 to all allies. */
void broadcast_shutdown(Maester *pstMaester);
/* Set SO_RCVTIMEO and SO_SNDTIMEO on nFd to nTimeoutSec seconds. */
void set_socket_timeout(int nFd, int nTimeoutSec);

#endif
