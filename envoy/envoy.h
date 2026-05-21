#ifndef __ENVOY_H__
#define __ENVOY_H__

/***********************************************
 *
 * @File:    envoy.h
 * @Purpose: Declares the Envoy structure, mission types, and all envoy lifecycle and dispatch functions.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE

#include "protocol.h"
#include "maester_types.h"
#include "inventory.h"
#include "pledge.h"
#include "pipe_msg.h"
#include <sys/types.h>

/* ── Mission types ───────────────────────────────────────────────────────── */

typedef enum {
    ENVOY_FREE    = 0,
    ENVOY_PLEDGE  = 1,
    ENVOY_LIST    = 2,
    ENVOY_TRADE   = 3,
    /* PLEDGE RESPOND ACCEPT/REJECT mission. */
    ENVOY_RESPOND = 4
} EnvoyMission;

/* ── Per-envoy state ─────────────────────────────────────────────────────── */

typedef struct {
    /* 1-based display number. */
    int          id;
    /* Current mission type. */
    EnvoyMission mission;
    /* Target realm name for ENVOY STATUS. */
    char         realm[50];
    /* Child PID; -1 if no child is running. */
    pid_t        pid;
    /* Parent's read end of result pipe; -1 when unused. */
    int          pipe_read_fd;
} Envoy;

/* ── Global mutexes (defined in envoy.c) ─────────────────────────────────── */

/* Protects the pledge table (pledges[], get/update/add functions). */
extern pthread_mutex_t g_pledge_mutex;

/* Protects products[], total_products, and the remote inventory cache. */
extern pthread_mutex_t g_data_mutex;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Allocate and initialise N envoy slots; store pointer to shared state.
 * Must be called before any other envoy function. */
void init_envoys(int nCount, Maester *pstMaester,
                 Product **ppstProducts, int *pnTotalProducts,
                 RemoteInventory **ppstCache, int *pnCacheCount);

/* Drain all active envoy pipes and wait for children (called on EXIT). */
void wait_all_envoys(void);

/* Free the envoy array (call after wait_all_envoys). */
void free_envoys(void);

/* ── Slot management ─────────────────────────────────────────────────────── */

/* Return the index of the first FREE envoy, or -1 if all occupied. */
int  find_free_envoy(void);

/* Reserve a slot (mission + realm set, no child started yet).
 * Used for START TRADE before the interactive sub-menu. */
void reserve_envoy(int nIdx, EnvoyMission eMission, char *psRealm);

/* Release a reserved-but-not-started slot (CANCEL path). */
void release_envoy(int nIdx);

/* ── Mission dispatch ─────────────────────────────────────────────────────── */

/* Fork a child for a PLEDGE mission. */
void dispatch_pledge(int nIdx, char *psRealm, char *psSigil);

/* Fork a child for a LIST PRODUCTS mission. */
void dispatch_list_products(int nIdx, char *psRealm);

/* Fork a child for a TRADE mission.
 * Takes ownership of pstItems (freed by the child on completion). */
void dispatch_trade(int nIdx, char *psRealm, TradeItem *pstItems, int nCount);

/* Fork a child for a PLEDGE RESPOND mission (ACCEPT or REJECT).
 * psResponse must be "ACCEPT" or "REJECT". */
void dispatch_pledge_respond(int nIdx, char *psRealm, char *psResponse);

/* ── Pledge-envoy lifecycle check ────────────────────────────────────────── */

/* Scan envoys in PLEDGE state whose child has exited (pid==-1) and release
 * those whose pledge is no longer OUTGOING_PENDING (resolved or timed out).
 * Call from the main select() loop alongside check_pledge_timeouts(). */
void check_pledge_envoys(void);

/* ── Pipe result handling (called from terminal select loop) ─────────────── */

/* Return the pipe read fd for envoy nIdx, or -1 if free. */
int get_envoy_pipe_fd(int nIdx);

/* Read one result from envoy nIdx's pipe and apply the state mutation.
 * Reaps the child and marks the slot FREE. */
void apply_envoy_result(int nIdx);

/* ── Status display ──────────────────────────────────────────────────────── */

void show_envoy_status(void);

/* ── Prompt notification (defined in terminal.c) ─────────────────────────── */
/* Called after printing async output so the main thread reprints '$ '. */
void notify_prompt(void);

#endif
