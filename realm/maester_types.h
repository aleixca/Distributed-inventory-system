#ifndef __MAESTER_TYPES_H__
#define __MAESTER_TYPES_H__

/***********************************************
 *
 * @File:    maester_types.h
 * @Purpose: Declares the Maester and Route structures and all configuration management functions.
 *           Implementation lives in maester.c (project root).
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE

#include "io.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

typedef struct {
    /* Destination realm name, or DEFAULT. */
    char *maester;
    /* Next-hop IPv4 string. */
    char *ip;
    /* Next-hop TCP port. */
    int   port;
} Route;

typedef struct {
    /* Local realm name. */
    char  *realm_name;
    /* Directory for sigils and temp files. */
    char  *user_dir;
    /* Number of outgoing child-process slots. */
    int    envoy_count;
    /* IP bound by the server socket. */
    char  *listen_ip;
    /* TCP port bound by the server socket. */
    int    listen_port;
    /* Dynamic routing table. */
    Route *routes;
    /* Number of route entries. */
    int    route_count;
    /* Legacy stop flag field. */
    int    stop_program;
    /* Legacy inventory count field. */
    int    total_products;
    /* Path to the binary stock file (argv[2]). */
    char  *stock_path;
} Maester;

Maester read_Maester(char *psPath);
void list_realms(Maester stMaester);
void exit_maester(Maester stMaester);
void free_Maester(Maester stMaester);
int  realm_exists(char *psRealm, Maester *pstMaester);

#endif
