#ifndef __PLEDGE_H__
#define __PLEDGE_H__

/***********************************************
 *
 * @File:    pledge.h
 * @Purpose: Declares the Pledge structure, status constants, and all pledge table management functions.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* Mutex protecting the pledge table — defined in pledge.c. */
extern pthread_mutex_t g_pledge_mutex;

#include <string.h>
#include <unistd.h>
#include <time.h>
#include "io.h"

#define MAX_PLEDGES      100
/* Max realm name length for pledge storage. */
#define REALM_NAME_SIZE   50
/* IPv4 max: "255.255.255.255\0". */
#define IP_SIZE           16

/* ── Status codes ────────────────────────────────────────────────────────── */
/* We sent PLEDGE and are awaiting response. */
#define PLEDGE_OUTGOING_PENDING  0
/* Alliance confirmed. */
#define PLEDGE_ALLIED            1
/* REJECT received or timeout. */
#define PLEDGE_FAILED            2
/* Ally sent 0x27 DISCONNECT. */
#define PLEDGE_INACTIVE          3
/* We received PLEDGE and user must respond. */
#define PLEDGE_INCOMING_PENDING  4

typedef struct {
    char   realm_name[REALM_NAME_SIZE];
    int    status;
    /* Ally's listening IP, known after alliance. */
    char   ip[IP_SIZE];
    /* Ally's listening port. */
    int    port;
    /* Set for OUTGOING_PENDING; 0 otherwise. */
    time_t sent_time;
} Pledge;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */
void  init_pledges(void);
int   add_outgoing_pledge(char *psRealm);
int   add_incoming_pledge(char *psRealm, char *psIp, int nPort);

/* ── Status queries ──────────────────────────────────────────────────────── */
void  show_pledge_status(void);
int   get_pledge_status(char *psRealm);
char *get_pledge_realm(int nIndex);
int   get_pledge_count(void);

/* ── Status updates ──────────────────────────────────────────────────────── */
void  update_pledge_status(char *psRealm, int nStatus);
void  update_pledge_ip_port(char *psRealm, char *psIp, int nPort);
int   get_pledge_ip_port(char *psRealm, char *psIpOut, int *pnPortOut);

/* ── Timeout ─────────────────────────────────────────────────────────────── */
void  check_pledge_timeouts(void);

#endif
