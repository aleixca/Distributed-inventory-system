/***********************************************
 *
 * @File:    commands.c
 * @Purpose: Parses user input from stdin into command constants and extracts realm/sigil arguments.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#include "commands.h"

/********************
 *
 * @Name: parse_command
 * @Def: Reads one line from stdin and parses it into a command constant.
 *       Sets *ppsRealm and *ppsSigil (both caller-owned, must be freed) when
 *       the command requires them. Both are set to NULL on entry.
 * @Arg: Out: ppsRealm = receives strdup'd realm name for commands that need it
 *       Out: ppsSigil = receives strdup'd sigil path for CMD_PLEDGE only
 * @Ret: CMD_* constant
 *
 ********************/
int parse_command(char **ppsRealm, char **ppsSigil) {
    char *psCommand = NULL;
    char *psW1, *psW2, *psW3, *psW4, *psW5;
    int   nResult   = CMD_UNKNOWN;
    char *psOrig    = NULL;
    char *psOrigW2  = NULL;
    char *psOrigW3  = NULL;

    if (NULL != ppsRealm) *ppsRealm = NULL;
    if (NULL != ppsSigil) *ppsSigil = NULL;

    /* read_screen() returns a heap string without the trailing newline. */
    psCommand = read_screen();

    if (NULL == psCommand) {
        return CMD_EXIT;
    }

    if (0 == strlen(psCommand)) {
        free(psCommand);
        return CMD_UNKNOWN;
    }

    /* Keep a copy in original case so file paths are not uppercased */
    /* psOrig preserves file paths/realm spelling while psCommand is normalised. */
    psOrig = strdup(psCommand);
    to_upper(psCommand);

    /* Tokenise the uppercased command for keyword comparisons. */
    psW1 = strtok(psCommand, " ");
    psW2 = strtok(NULL, " ");
    psW3 = strtok(NULL, " ");
    psW4 = strtok(NULL, " ");
    psW5 = strtok(NULL, " ");

    /* strtok() writes '\0' characters into psCommand at every delimiter,
     * so the token pointers (psW2, psW3, ...) point into psCommand, not
     * psOrig.  The byte offset of each token from the start of psCommand
     * is identical to its offset in psOrig (both were copied from the same
     * input string), so adding that offset to psOrig gives a pointer to
     * the same word but in its original, un-uppercased form.  This is why
     * realm names and file paths preserve their original capitalisation
     * even though psCommand has been fully uppercased. */
    if (NULL != psOrig && NULL != psW2) {
        size_t nOff = (size_t)(psW2 - psCommand);
        psOrigW2 = psOrig + nOff;
    }
    if (NULL != psOrig && NULL != psW3) {
        size_t nOff = (size_t)(psW3 - psCommand);
        psOrigW3 = psOrig + nOff;
    }

    if (NULL == psW1) {
        nResult = CMD_UNKNOWN;
    }

    else if (0 == strcmp(psW1, "EXIT")) {
        nResult = CMD_EXIT;
    }

    else if (0 == strcmp(psW1, "LIST")) {
        /* LIST REALMS, LIST PRODUCTS, or LIST PRODUCTS <realm>. */
        if (NULL == psW2) {
            nResult = CMD_INCOMPLETE_LIST;
        } else if (0 == strcmp(psW2, "REALMS")) {
            if (NULL != psW3) {
                nResult = CMD_UNKNOWN;
            } else {
                nResult = CMD_LIST_REALMS;
            }
        } else if (0 == strcmp(psW2, "PRODUCTS")) {
            if (NULL == psW3) {
                nResult = CMD_LIST_PRODUCTS_OWN;
            } else if (NULL == psW4) {
                nResult = CMD_LIST_PRODUCTS_REALM;
                if (NULL != ppsRealm) {
                    if (NULL != psOrigW3) {
                        *ppsRealm = strdup(psOrigW3);
                    } else {
                        *ppsRealm = strdup(psW3);
                    }
                }
            } else {
                nResult = CMD_UNKNOWN;
            }
        } else {
            nResult = CMD_UNKNOWN;
        }
    }

    else if (0 == strcmp(psW1, "PLEDGE")) {
        /* PLEDGE has status, response, and new-alliance variants. */
        if (NULL == psW2) {
            nResult = CMD_INCOMPLETE_PLEDGE;
        } else if (0 == strcmp(psW2, "STATUS")) {
            if (NULL != psW3) {
                nResult = CMD_UNKNOWN;
            } else {
                nResult = CMD_PLEDGE_STATUS;
            }
        } else if (0 == strcmp(psW2, "RESPOND")) {
            if (NULL == psW3 || NULL == psW4) {
                nResult = CMD_INCOMPLETE_PLEDGE_RESPOND;
            } else if (NULL != psW5) {
                nResult = CMD_UNKNOWN;
            } else if (0 == strcmp(psW4, "ACCEPT")) {
                nResult = CMD_PLEDGE_RESPOND_ACCEPT;
                if (NULL != ppsRealm) {
                    if (NULL != psOrigW3) {
                        /* strndup length is taken from the uppercased token
                         * psW3, not psOrigW3, because both point to the same
                         * word and strlen gives the correct character count
                         * regardless of case.  This caps the copy at exactly
                         * the token boundary, excluding any NUL-terminated
                         * remainder left in the buffer by strtok. */
                        *ppsRealm = strndup(psOrigW3, strlen(psW3));
                    } else {
                        *ppsRealm = strdup(psW3);
                    }
                }
            } else if (0 == strcmp(psW4, "REJECT")) {
                nResult = CMD_PLEDGE_RESPOND_REJECT;
                if (NULL != ppsRealm) {
                    if (NULL != psOrigW3) {
                        /* Same strndup boundary rationale as ACCEPT above. */
                        *ppsRealm = strndup(psOrigW3, strlen(psW3));
                    } else {
                        *ppsRealm = strdup(psW3);
                    }
                }
            } else {
                nResult = CMD_UNKNOWN;
            }
        } else {
            if (NULL == psW3) {
                nResult = CMD_INCOMPLETE_ALLIANCE;
            } else if (NULL != psW4) {
                nResult = CMD_UNKNOWN;
            } else {
                nResult = CMD_PLEDGE;
                if (NULL != ppsRealm) {
                    if (NULL != psOrigW2) {
                        *ppsRealm = strndup(psOrigW2, strlen(psW2));
                    } else {
                        *ppsRealm = strdup(psW2);
                    }
                }
                if (NULL != ppsSigil) {
                    if (NULL != psOrigW3) {
                        *ppsSigil = strdup(psOrigW3);
                    } else {
                        *ppsSigil = strdup(psW3);
                    }
                }
            }
        }
    }

    else if (0 == strcmp(psW1, "START")) {
        /* START TRADE <realm> enters the trade sub-prompt in terminal.c. */
        if (NULL == psW2) {
            nResult = CMD_INCOMPLETE_TRADE;
        } else if (0 != strcmp(psW2, "TRADE")) {
            nResult = CMD_UNKNOWN;
        } else if (NULL == psW3) {
            nResult = CMD_INCOMPLETE_TRADE;
        } else if (NULL != psW4) {
            nResult = CMD_UNKNOWN;
        } else {
            nResult = CMD_START_TRADE;
            if (NULL != ppsRealm) {
                if (NULL != psOrigW3) {
                    *ppsRealm = strdup(psOrigW3);
                } else {
                    *ppsRealm = strdup(psW3);
                }
            }
        }
    }

    else if (0 == strcmp(psW1, "ENVOY")) {
        /* ENVOY STATUS reports child-process slot state. */
        if (NULL == psW2) {
            nResult = CMD_INCOMPLETE_ENVOY;
        } else if (0 == strcmp(psW2, "STATUS") && NULL == psW3) {
            nResult = CMD_ENVOY_STATUS;
        } else {
            nResult = CMD_UNKNOWN;
        }
    }

    else {
        nResult = CMD_UNKNOWN;
    }

    free(psCommand);
    free(psOrig);
    return nResult;
}
