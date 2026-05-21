#ifndef __MESSAGE_HANDLER_H__
#define __MESSAGE_HANDLER_H__

/***********************************************
 *
 * @File:    message_handler.h
 * @Purpose: Declares the incoming frame dispatcher and all outgoing operation functions for the protocol.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/

#include "protocol.h"
#include "maester_types.h"
#include "inventory.h"
#include "pledge.h"

/********************
 * Incoming dispatcher: called from terminal's select() loop after recv_frame().
 * client_fd is still open; handlers may send responses on it.
 * terminal.c closes client_fd after this function returns.
 * products / total_products are double-indirected so handlers always read the
 * current pointer (realloc in send_trade_request may move the array).
 ********************/
void handle_incoming(Frame *pstFrame, int nClientFd, Maester *pstMaester, Product **ppstProducts, int *pnTotalProducts);

/********************
 * Outgoing operations (called from terminal command handlers):
 ********************/

/* Send PLEDGE (0x01) for sigil file to realm via routing table.
 * Returns 0 on success (pledge dispatched and MD5 OK), -1 on any failure.
 * Does NOT call add_outgoing_pledge — caller (parent process) handles that. */
int send_pledge(char *psRealm, char *psSigil, Maester *pstMaester);

/* Respond to an incoming PLEDGE: psResponse = "ACCEPT" or "REJECT".
 * Returns PLEDGE_ALLIED on successful ACCEPT, PLEDGE_FAILED otherwise.
 * Does NOT call update_pledge_status — caller (parent process) handles that. */
int send_pledge_response(char *psRealm, char *psResponse, Maester *pstMaester);

/* Request ally's product list (0x11); populate *ppstCache on success. */
void request_list_products(char *psRealm, Maester *pstMaester, Product *pstProducts, int nTotalProducts, RemoteInventory **ppstCache, int *pnCacheCount);

/* Send trade order to ally (0x14 + 0x15); update own stock on OK.
 * nEnvoyIdx is used to build a unique temp filename.
 * Returns 0 if order was accepted and stock updated, -1 otherwise. */
int send_trade_request(char *psRealm, Maester *pstMaester, Product **ppstProducts, int *pnTotalProducts, TradeItem *pstItems, int nItemCount, RemoteInventory **ppstCache, int *pnCacheCount, int nEnvoyIdx);

#endif
