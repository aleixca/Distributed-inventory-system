#ifndef __COMMANDS_H__
#define __COMMANDS_H__

/***********************************************
 *
 * @File:    commands.h
 * @Purpose: Declares command constants and the parse_command() function for terminal input handling.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/

#include "io.h"

#include <string.h>
#include <ctype.h>

/* Unrecognised syntax. */
#define CMD_UNKNOWN 0
/* LIST REALMS. */
#define CMD_LIST_REALMS 1
/* PLEDGE <realm> <sigil>. */
#define CMD_PLEDGE 2
/* PLEDGE RESPOND <realm> ACCEPT. */
#define CMD_PLEDGE_RESPOND_ACCEPT 3
/* PLEDGE RESPOND <realm> REJECT. */
#define CMD_PLEDGE_RESPOND_REJECT 4
/* LIST PRODUCTS. */
#define CMD_LIST_PRODUCTS_OWN 5
/* LIST PRODUCTS <realm>. */
#define CMD_LIST_PRODUCTS_REALM 6
/* START TRADE <realm>. */
#define CMD_START_TRADE 7
/* PLEDGE STATUS. */
#define CMD_PLEDGE_STATUS 8
/* ENVOY STATUS. */
#define CMD_ENVOY_STATUS 9
/* EXIT. */
#define CMD_EXIT 10
/* LIST missing subcommand. */
#define CMD_INCOMPLETE_LIST 11
/* PLEDGE missing subcommand/args. */
#define CMD_INCOMPLETE_PLEDGE 12
/* PLEDGE RESPOND missing args. */
#define CMD_INCOMPLETE_PLEDGE_RESPOND 13
/* PLEDGE missing realm/sigil. */
#define CMD_INCOMPLETE_ALLIANCE 14
/* START TRADE missing realm. */
#define CMD_INCOMPLETE_TRADE 15
/* ENVOY missing STATUS. */
#define CMD_INCOMPLETE_ENVOY 16

/* Reads stdin and fills optional arguments. */
int parse_command(char **ppsRealm, char **ppsSigil);

#endif
