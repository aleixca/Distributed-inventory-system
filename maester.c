/***********************************************
 *
 * @File:    maester.c
 * @Purpose: Main entry point and Maester configuration reading/saving.
 *           Combines program initialisation with realm config management.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "terminal.h"
#include "io.h"
#include "maester_types.h"
#include "inventory.h"
#include "pledge.h"
#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

/* ══════════════════════════════════════════════════════════════════════════
 *  MAESTER CONFIGURATION
 * ══════════════════════════════════════════════════════════════════════════ */

/* Remove all '&' characters from ps in-place. */
static void strip_amp(char *ps) {
    char *psR = ps, *psW = ps;

    /* Some statement/config examples decorate realm names with '&'.
     * Internally the program stores clean realm names so comparisons work. */
    while (*psR) {
        if (*psR != '&') {
            *psW++ = *psR;
        }
        psR++;
    }
    *psW = '\0';
}

/********************
 *
 * @Name: read_Maester
 * @Def: Reads a Maester configuration from a file.
 * @Arg: In: psPath = path to the configuration file
 * @Ret: A Maester structure populated with the configuration,
 *       or an empty structure (realm_name == NULL) on failure.
 *
 ********************/
Maester read_Maester(char *psPath) {
    Maester stMaester;
    char   *psBuf    = NULL;
    char   *psOutput = NULL;
    int     i        = 0;
    int     nFd;
    int     nLen;

    /* Start with a safe empty Maester. If any early error happens,
     * callers can detect failure because realm_name remains NULL. */
    stMaester.routes      = NULL;
    stMaester.route_count = 0;
    stMaester.stock_path  = NULL;
    stMaester.realm_name  = NULL;
    stMaester.user_dir    = NULL;
    stMaester.listen_ip   = NULL;

    /* Configuration files are parsed with low-level read helpers, not stdio. */
    nFd = open(psPath, O_RDONLY);

    if (-1 == nFd) {
        printF("Error opening file\n");
        return stMaester;
    }

    /* Line 1: realm name. */
    psBuf = readUntil(nFd, '\n');
    remove_slashr(psBuf);
    stMaester.realm_name = strdup(psBuf);
    strip_amp(stMaester.realm_name);
    free(psBuf);

    /* Line 2: user directory where sigils/temp files are stored. */
    psBuf = readUntil(nFd, '\n');
    remove_slashr(psBuf);
    stMaester.user_dir = strdup(psBuf);
    free(psBuf);
    /* Strip any leading slash — path is always relative to project root */
    if (NULL != stMaester.user_dir && '/' == stMaester.user_dir[0]) {
        char *psTmp = strdup(stMaester.user_dir + 1);
        free(stMaester.user_dir);
        stMaester.user_dir = psTmp;
    }

    /* Line 3: number of outgoing Envoy child-process slots. */
    psBuf = readUntil(nFd, '\n');
    remove_slashr(psBuf);
    stMaester.envoy_count = atoi(psBuf);
    free(psBuf);

    /* Line 4: local listening IP. */
    psBuf = readUntil(nFd, '\n');
    remove_slashr(psBuf);
    stMaester.listen_ip = strdup(psBuf);
    free(psBuf);

    /* Line 5: local listening TCP port. */
    psBuf = readUntil(nFd, '\n');
    remove_slashr(psBuf);
    stMaester.listen_port = atoi(psBuf);
    free(psBuf);

    /* Read trash line (---ROUTES---). */
    psBuf = readUntil(nFd, '\n');
    remove_slashr(psBuf);
    free(psBuf);

    /* Remaining lines are route table entries: realm IP port.
     * Reading one full line first prevents crashes when the route section
     * ends without DEFAULT or contains an incomplete/empty line. */
    while (NULL != (psBuf = readUntil(nFd, '\n'))) {
        Route  stTemp;
        Route *pstNewRoutes;
        char  *psRouteName;
        char  *psRouteIp;
        char  *psRoutePort;

        remove_slashr(psBuf);

        if ('\0' == psBuf[0]) {
            free(psBuf);
            continue;
        }

        psRouteName = strtok(psBuf, " \t");
        psRouteIp   = strtok(NULL, " \t");
        psRoutePort = strtok(NULL, " \t");

        if (NULL == psRouteName || NULL == psRouteIp || NULL == psRoutePort) {
            free(psBuf);
            continue;
        }

        strip_amp(psRouteName);
        stTemp.maester = strdup(psRouteName);
        stTemp.ip      = strdup(psRouteIp);
        stTemp.port    = atoi(psRoutePort);
        free(psBuf);

        /* Grow the dynamic route array one entry at a time. */
        pstNewRoutes = realloc(stMaester.routes, (i + 1) * sizeof(Route));
        if (NULL == pstNewRoutes) {
            printF("Error reallocating memory for routes\n");
            free(stMaester.routes);
            close(nFd);
            return stMaester;
        }
        stMaester.routes    = pstNewRoutes;
        stMaester.routes[i] = stTemp;
        i++;
    }
    stMaester.route_count = i;
    close(nFd);

    /* asprintf allocates a correctly-sized buffer for the formatted message,
     * avoiding a fixed-size stack buffer that could be too small for long
     * realm names. The -1 return-value check ensures we never pass NULL
     * to printF if allocation fails. */
    nLen = asprintf(&psOutput, "Maester of %s initialized. The board is set.\n",
                    stMaester.realm_name);
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }
    return stMaester;
}

/********************
 *
 * @Name: list_realms
 * @Def: Lists all realms in the Maester configuration.
 * @Arg: In: stMaester = Maester structure containing the configuration
 * @Ret: None
 *
 ********************/
void list_realms(Maester stMaester) {
    char *psOutput = NULL;
    int   nLen;
    int   i;

    /* The route table is the source of realms this Maester knows how to reach. */
    for (i = 0; i < stMaester.route_count; i++) {
        nLen = asprintf(&psOutput, " - %s\n", stMaester.routes[i].maester);
        if (-1 == nLen) {
            printF("Error creating output string\n");
            return;
        }
        printF(psOutput);
        free(psOutput);
    }
}

/********************
 *
 * @Name: exit_maester
 * @Def: Prints the sign-off message when the Maester shuts down.
 * @Arg: In: stMaester = Maester structure containing the configuration
 * @Ret: None
 *
 ********************/
void exit_maester(Maester stMaester) {
    char *psOutput = NULL;
    int   nLen;

    nLen = asprintf(&psOutput, "\nMaester of %s signing off. The board is cleared.\n",
                    stMaester.realm_name);
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }
}

/********************
 *
 * @Name: free_Maester
 * @Def: Frees all resources associated with a Maester structure.
 * @Arg: In: stMaester = Maester structure to free
 * @Ret: None
 *
 ********************/
void free_Maester(Maester stMaester) {
    int i;

    /* Every string in Maester is heap-owned by read_Maester(). */
    free(stMaester.realm_name);
    free(stMaester.user_dir);
    free(stMaester.listen_ip);
    free(stMaester.stock_path);
    for (i = 0; i < stMaester.route_count; i++) {
        free(stMaester.routes[i].maester);
        free(stMaester.routes[i].ip);
    }
    free(stMaester.routes);
}

/********************
 *
 * @Name: realm_exists
 * @Def: Checks whether psRealm appears as a named entry (not DEFAULT) in the
 *       Maester routing table.
 * @Arg: In: psRealm    = realm name to look for (case-insensitive)
 *       In: pstMaester = local Maester config
 * @Ret: 1 if found, 0 otherwise.
 *
 ********************/
int realm_exists(char *psRealm, Maester *pstMaester) {
    int i;

    /* DEFAULT is a routing fallback entry, not a real peer realm.
     * Skipping it prevents false positives when a caller checks
     * whether a specific named realm is known. */
    for (i = 0; i < pstMaester->route_count; i++) {
        if (0 == strcasecmp(pstMaester->routes[i].maester, "DEFAULT")) {
            continue;
        }
        if (0 == strcasecmp(pstMaester->routes[i].maester, psRealm)) {
            return 1;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ENTRY POINT
 * ══════════════════════════════════════════════════════════════════════════ */

volatile sig_atomic_t g_nStopFlag = 0;

static void handle_sigint(int nSig) {
    (void)nSig;
    /* SIGINT cannot safely run normal shutdown code, so it only sets a flag. */
    g_nStopFlag = 1;
    /* Closing stdin wakes select() in terminal(), allowing graceful cleanup. */
    /* Make select() return immediately. */
    close(STDIN_FILENO);
}

/********************
 *
 * @Name: main
 * @Def: Entry point. Loads config and inventory, opens the server socket,
 *       runs the terminal loop, then broadcasts shutdown and frees resources.
 * @Arg: In: argc = argument count
 *       In: argv = argument vector
 * @Ret: 0 on success, 1 on failure
 *
 ********************/
int main(int argc, char *argv[]) {
    int      nTotalProducts = 0;
    Product *pstProducts    = NULL;
    Maester  stMaester;
    int      nServerFd;

    if (argc != 3) {
        printF("Usage: ./maester <maester_config.txt> <inventory.bin>\n");
        return 1;
    }

    /* SIGINT is converted into a normal shutdown path. */
    signal(SIGINT,  handle_sigint);
    /* Broken sockets should return write errors instead of killing Maester. */
    /* Prevent process death on broken-pipe writes. */
    signal(SIGPIPE, SIG_IGN);
    init_pledges();

    /* First argument is the realm configuration file. */
    stMaester = read_Maester(argv[1]);
    if (NULL == stMaester.realm_name) {
        printF("Error: could not read configuration file.\n");
        return 1;
    }
    /* Second argument is the binary stock database used by inventory/trades. */
    stMaester.stock_path = strdup(argv[2]);
    pstProducts = load_inventory(argv[2], &nTotalProducts);

    if (NULL == pstProducts && 0 == nTotalProducts) {
        printF("Error loading inventory.\n");
        free_Maester(stMaester);
        return 1;
    }
    /* The server socket receives frames from other Maesters. */
    nServerFd = create_server_socket(stMaester.listen_ip, stMaester.listen_port);
    if (nServerFd < 0) {
        printF("Error: Could not create server socket.\n");
        free_inventory(pstProducts);
        free_Maester(stMaester);
        return 1;
    }

    /* terminal() owns the interactive loop until EXIT or CTRL+C. */
    terminal(&nTotalProducts, &pstProducts, &stMaester, nServerFd, &g_nStopFlag);

    /* If we got here via CTRL+C, print the signing-off message that the
     * EXIT command would normally trigger via exit_maester(). */
    if (g_nStopFlag) {
        exit_maester(stMaester);
    }

    broadcast_shutdown(&stMaester);
    close(nServerFd);
    free_inventory(pstProducts);
    free_Maester(stMaester);

    return 0;
}
