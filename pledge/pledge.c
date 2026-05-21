/***********************************************
 *
 * @File:    pledge.c
 * @Purpose: Manages the pledge table tracking alliance states, timeouts, and IP/port information for all realms.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "pledge.h"
#include <strings.h>

static Pledge g_stPledges[MAX_PLEDGES];
static int    g_nPledgeCount = 0;

/* g_pledge_mutex is declared extern in pledge.h and used by envoy threads
 * and connection threads.  Defined here (not in envoy.c) so the pledge
 * module fully owns its synchronisation.
 *
 * Recursive so callers can hold the lock over multiple pledge-table
 * operations atomically (e.g. update_status + update_ip_port together)
 * without deadlocking when the same lock is re-entered inside. */
pthread_mutex_t g_pledge_mutex;


/* Internal helper: find index of realm — MUST be called with lock held */
static int find_pledge(char *psRealm) {
    int i;

    /* Case-insensitive because user commands may use different capitalisation. */
    for (i = 0; i < g_nPledgeCount; i++) {
        if (0 == strcasecmp(g_stPledges[i].realm_name, psRealm)) {
            return i;
        }
    }
    return -1;
}

/********************
 *
 * @Name: init_pledges
 * @Def: Initialises the pledge array and the recursive pledge mutex.
 * @Arg: None
 * @Ret: None
 *
 ********************/
void init_pledges(void) {
    int                 i;
    pthread_mutexattr_t stAttr;

    /* PTHREAD_MUTEX_RECURSIVE allows the same thread to lock g_pledge_mutex
     * more than once without deadlocking itself.  This is needed because callers
     * such as handle_alliance_resp() lock the mutex, then call update_pledge_status()
     * and update_pledge_ip_port() which each try to lock it again internally.
     * A default (non-recursive) mutex would deadlock on the second lock call. */
    pthread_mutexattr_init(&stAttr);
    pthread_mutexattr_settype(&stAttr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_pledge_mutex, &stAttr);
    pthread_mutexattr_destroy(&stAttr);

    g_nPledgeCount = 0;
    for (i = 0; i < MAX_PLEDGES; i++) {
        g_stPledges[i].realm_name[0] = '\0';
        g_stPledges[i].status        = PLEDGE_OUTGOING_PENDING;
        g_stPledges[i].ip[0]         = '\0';
        g_stPledges[i].port          = 0;
        g_stPledges[i].sent_time     = 0;
    }
}

/********************
 *
 * @Name: add_outgoing_pledge
 * @Def: Records a PLEDGE we sent. Sets status OUTGOING_PENDING + timestamp.
 *       If the realm already exists (re-pledge after FAILED), resets it.
 * @Arg: In: psRealm = target realm name
 * @Ret: 0 on success, -1 if the array is full
 *
 ********************/
int add_outgoing_pledge(char *psRealm) {
    int nIdx;

    /* All pledge-table accesses are protected: network threads and terminal
     * Envoys may query/update this table at the same time. */
    pthread_mutex_lock(&g_pledge_mutex);
    nIdx = find_pledge(psRealm);
    if (-1 == nIdx) {
        if (g_nPledgeCount >= MAX_PLEDGES) {
            pthread_mutex_unlock(&g_pledge_mutex);
            return -1;
        }
        nIdx = g_nPledgeCount++;
    }
    strncpy(g_stPledges[nIdx].realm_name, psRealm, REALM_NAME_SIZE - 1);
    g_stPledges[nIdx].realm_name[REALM_NAME_SIZE - 1] = '\0';
    g_stPledges[nIdx].status    = PLEDGE_OUTGOING_PENDING;
    g_stPledges[nIdx].ip[0]     = '\0';
    g_stPledges[nIdx].port      = 0;
    /* sent_time drives the 120-second timeout in check_pledge_timeouts().
     * Recording it here (at send time, not at response time) means the clock
     * starts the moment the Envoy dispatches the PLEDGE, not when we notice
     * there is no reply — giving a true end-to-end deadline. */
    g_stPledges[nIdx].sent_time = time(NULL);
    pthread_mutex_unlock(&g_pledge_mutex);
    return 0;
}

/********************
 *
 * @Name: add_incoming_pledge
 * @Def: Records a PLEDGE we received. Sets INCOMING_PENDING + origin address.
 * @Arg: In: psRealm = origin realm name
 *       In: psIp    = origin's listening IP
 *       In: nPort   = origin's listening port
 * @Ret: 0 on success, -1 if the array is full
 *
 ********************/
int add_incoming_pledge(char *psRealm, char *psIp, int nPort) {
    int nIdx;

    /* Incoming pledge stores origin address so response can be direct. */
    pthread_mutex_lock(&g_pledge_mutex);
    nIdx = find_pledge(psRealm);
    if (-1 == nIdx) {
        if (g_nPledgeCount >= MAX_PLEDGES) {
            pthread_mutex_unlock(&g_pledge_mutex);
            return -1;
        }
        nIdx = g_nPledgeCount++;
    }
    strncpy(g_stPledges[nIdx].realm_name, psRealm, REALM_NAME_SIZE - 1);
    g_stPledges[nIdx].realm_name[REALM_NAME_SIZE - 1] = '\0';
    g_stPledges[nIdx].status    = PLEDGE_INCOMING_PENDING;
    /* Store the sender's listen address (parsed from the ORIGIN field of the
     * 0x01 frame) so that send_pledge_response() can connect back directly
     * instead of routing through the relay chain again. */
    strncpy(g_stPledges[nIdx].ip, psIp, IP_SIZE - 1);
    g_stPledges[nIdx].ip[IP_SIZE - 1] = '\0';
    g_stPledges[nIdx].port      = nPort;
    /* sent_time is 0 for incoming pledges — timeout only applies to pledges
     * we sent, not ones we received; check_pledge_timeouts skips 0 values. */
    g_stPledges[nIdx].sent_time = 0;
    pthread_mutex_unlock(&g_pledge_mutex);
    return 0;
}

/********************
 *
 * @Name: show_pledge_status
 * @Def: Prints all pledges with their current status.
 * @Arg: None
 * @Ret: None
 *
 ********************/
void show_pledge_status(void) {
    char *psOutput    = NULL;
    char *psStatusStr = NULL;
    int   nLen;
    int   i;

    /* Snapshot status text while holding the lock. */
    pthread_mutex_lock(&g_pledge_mutex);

    if (0 == g_nPledgeCount) {
        pthread_mutex_unlock(&g_pledge_mutex);
        printF("You have no pledges awaiting or accepted\n");
        return;
    }

    for (i = 0; i < g_nPledgeCount; i++) {
        switch (g_stPledges[i].status) {
            case PLEDGE_OUTGOING_PENDING:  psStatusStr = "PENDING";            break;
            case PLEDGE_ALLIED:            psStatusStr = "ACCEPTED";           break;
            case PLEDGE_FAILED:            psStatusStr = "REJECTED";           break;
            case PLEDGE_INACTIVE:          psStatusStr = "INACTIVE";           break;
            case PLEDGE_INCOMING_PENDING:  psStatusStr = "PENDING (incoming)"; break;
            default:                       psStatusStr = "UNKNOWN";            break;
        }
        nLen = asprintf(&psOutput, "- %s: %s\n",
                        g_stPledges[i].realm_name, psStatusStr);
        if (-1 != nLen) {
            printF(psOutput);
            free(psOutput);
        }
    }

    pthread_mutex_unlock(&g_pledge_mutex);
}

/********************
 *
 * @Name: get_pledge_status
 * @Def: Returns the status constant for the given realm, or -1 if not found.
 * @Arg: In: psRealm = realm name to look up
 * @Ret: Status constant, or -1 if not found
 *
 ********************/
int get_pledge_status(char *psRealm) {
    int nResult;
    int nIdx;

    /* Simple getter still locks because writers may run in other threads. */
    pthread_mutex_lock(&g_pledge_mutex);
    nIdx = find_pledge(psRealm);
    if (-1 == nIdx) {
        nResult = -1;
    } else {
        nResult = g_stPledges[nIdx].status;
    }
    pthread_mutex_unlock(&g_pledge_mutex);
    return nResult;
}

/********************
 *
 * @Name: update_pledge_status
 * @Def: Sets a new status for the given realm's pledge entry.
 * @Arg: In: psRealm = realm name to update
 *       In: nStatus = new status constant
 * @Ret: None
 *
 ********************/
void update_pledge_status(char *psRealm, int nStatus) {
    int nIdx;

    /* Status transitions: pending -> allied/failed/inactive. */
    pthread_mutex_lock(&g_pledge_mutex);
    nIdx = find_pledge(psRealm);
    if (-1 != nIdx) {
        g_stPledges[nIdx].status = nStatus;
    }
    pthread_mutex_unlock(&g_pledge_mutex);
}

/********************
 *
 * @Name: update_pledge_ip_port
 * @Def: Stores the direct IP and port for an existing pledge entry.
 * @Arg: In: psRealm = realm name to update
 *       In: psIp    = IP address string
 *       In: nPort   = port number
 * @Ret: None
 *
 ********************/
void update_pledge_ip_port(char *psRealm, char *psIp, int nPort) {
    int nIdx;

    /* Direct address is learned when an alliance becomes usable. */
    pthread_mutex_lock(&g_pledge_mutex);
    nIdx = find_pledge(psRealm);
    if (-1 != nIdx) {
        strncpy(g_stPledges[nIdx].ip, psIp, IP_SIZE - 1);
        g_stPledges[nIdx].ip[IP_SIZE - 1] = '\0';
        g_stPledges[nIdx].port = nPort;
    }
    pthread_mutex_unlock(&g_pledge_mutex);
}

/********************
 *
 * @Name: get_pledge_ip_port
 * @Def: Retrieves the stored IP and port for a realm.
 * @Arg: In:  psRealm   = realm name to look up
 *       Out: psIpOut   = buffer to receive the IP string
 *       Out: pnPortOut = pointer to receive the port number
 * @Ret: 0 on success, -1 if not found or ip unknown.
 *
 ********************/
int get_pledge_ip_port(char *psRealm, char *psIpOut, int *pnPortOut) {
    int nResult;
    int nIdx;

    /* Returns the address that Envoys use for direct ally communication. */
    pthread_mutex_lock(&g_pledge_mutex);
    nIdx = find_pledge(psRealm);
    if (-1 == nIdx || g_stPledges[nIdx].ip[0] == '\0') {
        nResult = -1;
    } else {
        strncpy(psIpOut, g_stPledges[nIdx].ip, IP_SIZE - 1);
        psIpOut[IP_SIZE - 1] = '\0';
        *pnPortOut = g_stPledges[nIdx].port;
        nResult    = 0;
    }
    pthread_mutex_unlock(&g_pledge_mutex);
    return nResult;
}

/********************
 *
 * @Name: get_pledge_realm
 * @Def: Returns the realm name at the given index, or NULL if out of range.
 * @Arg: In: nIndex = index into the pledge table
 * @Ret: Pointer to the realm name string, or NULL
 *
 ********************/
char *get_pledge_realm(int nIndex) {
    char *psResult;

    /* Returns a pointer directly into the internal array — valid only while the
     * caller holds or has just released the lock.  Callers (e.g. check_pledge_envoys)
     * use this pointer immediately to call get_pledge_status(), which re-acquires
     * the lock itself; the recursive mutex allows that without deadlock.
     * Callers must NOT store this pointer beyond a single locked region. */
    pthread_mutex_lock(&g_pledge_mutex);
    if (nIndex >= 0 && nIndex < g_nPledgeCount) {
        psResult = g_stPledges[nIndex].realm_name;
    } else {
        psResult = NULL;
    }
    pthread_mutex_unlock(&g_pledge_mutex);
    return psResult;
}

/********************
 *
 * @Name: get_pledge_count
 * @Def: Returns the total number of pledge entries.
 * @Arg: None
 * @Ret: Number of pledges
 *
 ********************/
int get_pledge_count(void) {
    int nResult;

    /* Count can change when incoming/outgoing pledges are added. */
    pthread_mutex_lock(&g_pledge_mutex);
    nResult = g_nPledgeCount;
    pthread_mutex_unlock(&g_pledge_mutex);
    return nResult;
}

/********************
 *
 * @Name: check_pledge_timeouts
 * @Def: Scans OUTGOING_PENDING pledges; marks those >= 120 seconds old as FAILED.
 * @Arg: None
 * @Ret: None
 *
 ********************/
void check_pledge_timeouts(void) {
    time_t tNow     = time(NULL);
    char  *psOutput = NULL;
    int    nLen;
    int    i;

    /* Timeout scan runs periodically from terminal's select() loop. */
    pthread_mutex_lock(&g_pledge_mutex);

    for (i = 0; i < g_nPledgeCount; i++) {
        if (PLEDGE_OUTGOING_PENDING != g_stPledges[i].status) {
            continue;
        }
        if (0 == g_stPledges[i].sent_time) {
            continue;
        }
        if (tNow - g_stPledges[i].sent_time >= 120) {
            g_stPledges[i].status = PLEDGE_FAILED;
            nLen = asprintf(&psOutput,
                "\n>>> Pledge to %s has failed (TIMEOUT).\n",
                g_stPledges[i].realm_name);
            if (-1 != nLen) {
                printF(psOutput);
                free(psOutput);
            }
        }
    }

    pthread_mutex_unlock(&g_pledge_mutex);
}
