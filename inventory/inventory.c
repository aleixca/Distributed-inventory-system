/***********************************************
 *
 * @File:    inventory.c
 * @Purpose: Implements product loading, listing, stock file operations, remote inventory caching, and trade item collection.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE
#include "inventory.h"
/* g_data_mutex protects inventory/cache updates. */
#include "envoy.h"
#include <strings.h>
#include <pthread.h>

/* ══════════════════════════════════════════════════════════════════════════
 *  PRODUCT
 * ══════════════════════════════════════════════════════════════════════════ */

/********************
 *
 * @Name: load_inventory
 * @Def: Reads the binary stock database into a dynamic Product array.
 *       The file is expected to contain consecutive Product structs.
 * @Arg: In:  psPath          = stock.db path
 *       Out: pnTotalProducts = number of products loaded
 * @Ret: Heap Product array, or NULL on open/allocation failure.
 *
 ********************/
Product *load_inventory(char *psPath, int *pnTotalProducts) {
    int      nFd          = open(psPath, O_RDONLY);
    int      i            = 0;
    Product *pstProducts  = NULL;
    Product  stTemp;

    if (-1 == nFd) {
        printF("Error: Could not open inventory file.\n");
        return NULL;
    }

    /* The stock file is a flat binary sequence of Product structs with no
     * header or length prefix — records are identified purely by their fixed
     * sizeof(Product) stride.  Reading exactly sizeof(Product) bytes per
     * iteration is how we parse and count records without a separate index. */
    while (0 != read(nFd, &stTemp, sizeof(Product))) {
        Product *pstNewProducts = realloc(pstProducts, (i + 1) * sizeof(Product));
        if (NULL == pstNewProducts) {
            printF("Error: Could not reallocate memory for products.\n");
            free(pstProducts);
            close(nFd);
            return NULL;
        }
        pstProducts    = pstNewProducts;
        pstProducts[i] = stTemp;
        i++;
    }

    close(nFd);
    *pnTotalProducts = i;
    return pstProducts;
}

/********************
 *
 * @Name: list_products
 * @Def: Prints a formatted view of a Product array.
 * @Arg: In: nTotalProducts = number of products to print
 *       In: pstProducts    = array to display
 * @Ret: None
 *
 ********************/
void list_products(int nTotalProducts, Product *pstProducts) {
    char *psOutput = NULL;
    char *psHeader = NULL;
    int   nLen;
    int   i;

    printF("--- Trade Ledger --- \n");
    nLen = asprintf(&psHeader, "%-25s | %15s | %12s\n",
                    "Item", "Value (Gold)", "Weight (Stone)");
    if (-1 != nLen) {
        printF(psHeader);
        free(psHeader);
    }
    printF("-------------------------------------------------------\n");

    for (i = 0; i < nTotalProducts; i++) {
        nLen = asprintf(&psOutput, "%-25s | %15d | %12.1f\n",
                        pstProducts[i].name, pstProducts[i].amount,
                        pstProducts[i].weight);
        if (-1 == nLen) {
            printF("Error creating output string\n");
            return;
        }
        printF(psOutput);
        free(psOutput);
    }
    printF("\n-------------------------------------------------------\n");
    nLen = asprintf(&psOutput, "Total Entries: %d\n", nTotalProducts);
    if (-1 == nLen) {
        printF("Error creating output string\n");
        return;
    }
    printF(psOutput);
    free(psOutput);
}

/********************
 *
 * @Name: free_inventory
 * @Def: Releases the Product array loaded from disk or grown by trades.
 * @Arg: In: pstProducts = heap inventory pointer
 * @Ret: None
 *
 ********************/
void free_inventory(Product *pstProducts) {
    free(pstProducts);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  STOCK FILE OPERATIONS
 * ══════════════════════════════════════════════════════════════════════════ */

/********************
 *
 * @Name: write_stock_file
 * @Def: Persists the current Product array back to the binary stock database.
 * @Arg: In: psPath      = stock.db path
 *       In: pstProducts = current in-memory inventory
 *       In: nCount      = number of products
 * @Ret: 0 on success, -1 on write/open error.
 *
 ********************/
int write_stock_file(char *psPath, Product *pstProducts, int nCount) {
    int nFd;
    int i;

    nFd = open(psPath, O_WRONLY | O_TRUNC, 0644);
    if (-1 == nFd) {
        return -1;
    }

    /* Rewrite the whole file so disk matches the current memory state. */
    for (i = 0; i < nCount; i++) {
        if (-1 == write(nFd, &pstProducts[i], sizeof(Product))) {
            close(nFd);
            return -1;
        }
    }
    close(nFd);
    return 0;
}

/********************
 *
 * @Name: apply_stock_delta
 * @Def: Adds nDelta to an existing product amount.
 *       Negative deltas are used when fulfilling an incoming order.
 * @Arg: In/Out: pstProducts = inventory array
 *       In:     nCount      = number of products
 *       In:     psName      = product name to modify
 *       In:     nDelta      = signed amount change
 * @Ret: 0 on success, -1 if product missing or amount would become negative.
 *
 ********************/
int apply_stock_delta(Product *pstProducts, int nCount, char *psName, int nDelta) {
    int i;

    for (i = 0; i < nCount; i++) {
        if (0 == strcasecmp(pstProducts[i].name, psName)) {
            int nNewAmount = pstProducts[i].amount + nDelta;
            /* A signed delta lets the same function handle both gaining stock
             * (incoming trade, positive nDelta) and deducting stock (fulfilling
             * an outgoing order, negative nDelta).  The negative check prevents
             * selling more than what is actually held. */
            if (nNewAmount < 0) {
                return -1;
            }
            pstProducts[i].amount = nNewAmount;
            return 0;
        }
    }
    return -1;
}

/********************
 *
 * @Name: add_or_update_product
 * @Def: Adds units to an existing product, or creates a new product if missing.
 *       Used by the parent after a successful outgoing trade.
 * @Arg: In/Out: ppstProducts = inventory pointer, may be realloced
 *       In/Out: pnCount      = product count
 *       In:     psName       = product name
 *       In:     nAmount      = amount gained
 *       In:     fWeight      = product weight copied from ally cache
 * @Ret: 0 on success, -1 on allocation failure.
 *
 ********************/
int add_or_update_product(Product **ppstProducts, int *pnCount,
                           const char *psName, int nAmount, float fWeight) {
    int      i;
    Product *pstNp;

    /* Existing product: only amount changes. */
    for (i = 0; i < *pnCount; i++) {
        if (0 == strcasecmp((*ppstProducts)[i].name, psName)) {
            (*ppstProducts)[i].amount += nAmount;
            return 0;
        }
    }

    /* New product: grow the inventory array by one slot. */
    pstNp = realloc(*ppstProducts, (size_t)(*pnCount + 1) * sizeof(Product));
    if (NULL == pstNp) {
        return -1;
    }
    *ppstProducts = pstNp;
    memset(&(*ppstProducts)[*pnCount], 0, sizeof(Product));
    strncpy((*ppstProducts)[*pnCount].name, psName, 99);
    (*ppstProducts)[*pnCount].name[99]   = '\0';
    (*ppstProducts)[*pnCount].amount     = nAmount;
    (*ppstProducts)[*pnCount].weight     = fWeight;
    (*pnCount)++;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  REMOTE INVENTORY CACHE
 * ══════════════════════════════════════════════════════════════════════════ */

/********************
 *
 * @Name: ri_set_products
 * @Def: Stores/replaces the cached product list for one remote realm.
 * @Arg: In/Out: pstCache     = remote cache array, may be realloced
 *       In/Out: pnCacheCount = number of cached realms
 *       In:     psRealm      = realm whose products are being cached
 *       In:     pstProducts  = source product list
 *       In:     nCount       = number of source products
 * @Ret: Updated cache pointer; unchanged pointer on allocation failure.
 *
 ********************/
RemoteInventory *ri_set_products(RemoteInventory *pstCache, int *pnCacheCount,
                                  char *psRealm, Product *pstProducts, int nCount) {
    int              i;
    int              nIdx = -1;
    RemoteInventory *pstNewCache;

    /* First check if this realm already has a cache entry. */
    for (i = 0; i < *pnCacheCount; i++) {
        if (0 == strcasecmp(pstCache[i].realm_name, psRealm)) {
            nIdx = i;
            break;
        }
    }

    if (-1 == nIdx) {
        /* New realm cache entry. */
        pstNewCache = realloc(pstCache,
                              (size_t)(*pnCacheCount + 1) * sizeof(RemoteInventory));
        if (NULL == pstNewCache) {
            return pstCache;
        }
        pstCache = pstNewCache;
        nIdx     = *pnCacheCount;
        (*pnCacheCount)++;
        strncpy(pstCache[nIdx].realm_name, psRealm, REALM_NAME_SIZE - 1);
        pstCache[nIdx].realm_name[REALM_NAME_SIZE - 1] = '\0';
        pstCache[nIdx].products = NULL;
        pstCache[nIdx].count    = 0;
    } else {
        /* Existing realm: replace old product snapshot. */
        free(pstCache[nIdx].products);
        pstCache[nIdx].products = NULL;
    }

    /* Deep-copy the product list so the cache owns its own memory independent
     * of the Envoy's temporary parse buffer, which will be freed after this
     * call returns.  The caller (list_handler) must not access pstProducts
     * after passing it here. */
    if (nCount > 0) {
        pstCache[nIdx].products = malloc((size_t)nCount * sizeof(Product));
        if (NULL != pstCache[nIdx].products) {
            memcpy(pstCache[nIdx].products, pstProducts,
                   (size_t)nCount * sizeof(Product));
        }
    }
    /* Only update count after confirming the allocation succeeded; a failed
     * malloc leaves count=0 so the cache entry is safely treated as empty. */
    if (NULL != pstCache[nIdx].products) {
        pstCache[nIdx].count = nCount;
    } else {
        pstCache[nIdx].count = 0;
    }
    return pstCache;
}

/********************
 *
 * @Name: ri_get_products
 * @Def: Returns the cached product list for one remote realm.
 * @Arg: In:  pstCache    = remote cache array
 *       In:  nCacheCount = number of cached realms
 *       In:  psRealm     = realm to search
 *       Out: pnCountOut  = number of products in returned list
 * @Ret: Pointer owned by cache, or NULL if not cached.
 *
 ********************/
Product *ri_get_products(RemoteInventory *pstCache, int nCacheCount,
                          char *psRealm, int *pnCountOut) {
    int i;

    *pnCountOut = 0;
    for (i = 0; i < nCacheCount; i++) {
        if (0 == strcasecmp(pstCache[i].realm_name, psRealm)) {
            *pnCountOut = pstCache[i].count;
            return pstCache[i].products;
        }
    }
    return NULL;
}

/********************
 *
 * @Name: ri_free_all
 * @Def: Frees all cached remote inventories and the cache array itself.
 * @Arg: In: pstCache    = cache array
 *       In: nCacheCount = number of entries
 * @Ret: None
 *
 ********************/
void ri_free_all(RemoteInventory *pstCache, int nCacheCount) {
    int i;

    if (NULL == pstCache) {
        return;
    }
    for (i = 0; i < nCacheCount; i++) {
        free(pstCache[i].products);
    }
    free(pstCache);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  TRADE
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Non-blocking trade session ─────────────────────────────────────────── */

static int valid_product_for_realm(char *psProduct,
                                    Product *pstAllyProducts, int nAllyCount);

/********************
 *
 * @Name: trade_session_init
 * @Def: Starts interactive trade mode using a snapshot of the ally inventory.
 *       The snapshot avoids reading a cache entry while another Envoy changes it.
 * @Arg: Out: pstSess      = trade session state to initialise
 *       In:  psRealm      = ally being traded with
 *       In/Out: ppstCache = remote inventory cache
 *       In/Out: pnCacheCount = number of cache entries
 * @Ret: 0 on success, -1 if no products are cached for that ally.
 *
 ********************/
int trade_session_init(TradeSession *pstSess, char *psRealm,
                       RemoteInventory **ppstCache, int *pnCacheCount) {
    int      nAllyCount      = 0;
    Product *pstAllyProducts = NULL;
    char    *psOutput        = NULL;
    int      nLen;
    int      i;

    memset(pstSess, 0, sizeof(TradeSession));
    strncpy(pstSess->sRealm, psRealm, 49);
    pstSess->sRealm[49] = '\0';

    /* Take a private snapshot of the ally's cached product list under the
     * mutex, then release it before the interactive prompt loop begins.
     * Holding the mutex during user input would block any LIST PRODUCTS Envoy
     * that tries to refresh the cache while the user is typing — a potential
     * deadlock.  The snapshot is safe to read without the lock after this point
     * because it is a private malloc'd copy, not the shared cache pointer. */
    pthread_mutex_lock(&g_data_mutex);
    {
        Product *pstSrc = ri_get_products(*ppstCache, *pnCacheCount,
                                          psRealm, &nAllyCount);
        if (NULL != pstSrc && nAllyCount > 0) {
            pstAllyProducts = malloc((size_t)nAllyCount * sizeof(Product));
            if (NULL != pstAllyProducts) {
                memcpy(pstAllyProducts, pstSrc,
                       (size_t)nAllyCount * sizeof(Product));
            } else {
                nAllyCount = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_data_mutex);

    if (NULL == pstAllyProducts) {
        printF("No products available. Use LIST PRODUCTS first.\n");
        return -1;
    }

    /* trade_session_input() and trade_session_free() are called from the
     * terminal select() loop without holding any mutex, keeping the main loop
     * non-blocking.  The split into init/input/free replaces the old blocking
     * collect_trade_items() which called read_screen() inside a while(1) loop
     * and could not interleave with incoming connection events. */
    pstSess->pstAllyProducts = pstAllyProducts;
    pstSess->nAllyCount      = nAllyCount;
    pstSess->pstItems        = NULL;
    pstSess->nCount          = 0;

    nLen = asprintf(&psOutput,
        "Trade with %s begins.\n"
        "A direct path is open; your houses are allied, "
        "and no intermediaries stand in between.\n", psRealm);
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }

    printF("Available products: ");
    for (i = 0; i < nAllyCount; i++) {
        printF(pstAllyProducts[i].name);
        if (i < nAllyCount - 1) {
            printF(", ");
        }
    }
    printF("\n");
    return 0;
}

/********************
 *
 * @Name: trade_session_input
 * @Def: Processes one command inside trade mode.
 *       Supported commands: ADD, REMOVE, SEND, CANCEL.
 * @Arg: In/Out: pstSess = current trade session
 *       In/Out: psLine  = input line, modified by strtok
 * @Ret: 0 continue, 1 send trade, -1 cancel.
 *
 ********************/
int trade_session_input(TradeSession *pstSess, char *psLine) {
    char *psW1   = NULL;
    char *psItem = NULL;
    int   nAmount;
    int   i;

    if (0 == strcmp(psLine, "CANCEL")) {
        printF("Trade cancelled.\n");
        return -1;
    }

    if (0 == strcmp(psLine, "SEND")) {
        if (0 == pstSess->nCount) {
            printF("No items selected. Add items before sending.\n");
            return 0;
        }
        return 1;
    }

    psW1 = strtok(psLine, " ");
    if (NULL == psW1) {
        printF("Unknown command. Available: ADD <PRODUCT> <AMOUNT>, "
               "REMOVE <PRODUCT> <AMOUNT>, SEND, CANCEL\n");
        return 0;
    }

    if (0 == strcmp(psW1, "ADD") || 0 == strcmp(psW1, "REMOVE")) {
        char *psToks[7];
        int   nNtoks = 0;
        char *psTok  = strtok(NULL, " ");

        /* Product name may contain spaces; final token is the amount. */
        while (NULL != psTok && nNtoks < 7) {
            psToks[nNtoks++] = psTok;
            psTok = strtok(NULL, " ");
        }

        if (nNtoks < 2 || !is_number(psToks[nNtoks - 1])) {
            printF("Invalid command. Usage: ADD/REMOVE <PRODUCT> <AMOUNT>\n");
            return 0;
        }

        nAmount = atoi(psToks[nNtoks - 1]);

        {
            size_t nNameLen = 0;
            int    j;
            /* Rebuild the product name from all tokens except the amount. */
            for (j = 0; j < nNtoks - 1; j++) {
                nNameLen += strlen(psToks[j]) + 1;
            }
            psItem = malloc(nNameLen);
            if (NULL == psItem) {
                return -1;
            }
            psItem[0] = '\0';
            for (j = 0; j < nNtoks - 1; j++) {
                if (j > 0) {
                    strcat(psItem, " ");
                }
                strcat(psItem, psToks[j]);
            }
        }

        if (nAmount <= 0) {
            printF("Invalid amount. Must be a positive integer.\n");
            free(psItem);
            return 0;
        }

        if (!valid_product_for_realm(psItem, pstSess->pstAllyProducts,
                                     pstSess->nAllyCount)) {
            printF("Product not available from this realm.\n");
            free(psItem);
            return 0;
        }

        if (0 == strcmp(psW1, "ADD")) {
            int nIndex = find_product(pstSess->pstItems,
                                      pstSess->nCount, psItem);
            if (-1 != nIndex) {
                pstSess->pstItems[nIndex].amount += nAmount;
            } else {
                TradeItem *pstNew = realloc(
                    pstSess->pstItems,
                    (size_t)(pstSess->nCount + 1) * sizeof(TradeItem));
                if (NULL == pstNew) {
                    free(psItem);
                    return -1;
                }
                pstSess->pstItems                       = pstNew;
                pstSess->pstItems[pstSess->nCount].name = strdup(psItem);
                if (NULL == pstSess->pstItems[pstSess->nCount].name) {
                    free(psItem);
                    return -1;
                }
                pstSess->pstItems[pstSess->nCount].amount = nAmount;
                pstSess->nCount++;
            }
            printF("Product added to trade list.\n");
        } else {
            int nIndex = find_product(pstSess->pstItems,
                                      pstSess->nCount, psItem);
            if (-1 == nIndex) {
                printF("Product not in trade list.\n");
            } else if (pstSess->pstItems[nIndex].amount < nAmount) {
                printF("Cannot remove more than the current amount.\n");
            } else {
                pstSess->pstItems[nIndex].amount -= nAmount;
                /* When an item reaches zero, compact the array in place by
                 * shifting subsequent entries left, then shrink the allocation.
                 * The name was strdup'd on ADD, so it must be freed separately
                 * before the slot is overwritten by the shift. */
                if (0 == pstSess->pstItems[nIndex].amount) {
                    free(pstSess->pstItems[nIndex].name);
                    for (i = nIndex; i < pstSess->nCount - 1; i++) {
                        pstSess->pstItems[i] = pstSess->pstItems[i + 1];
                    }
                    pstSess->nCount--;
                    if (0 == pstSess->nCount) {
                        free(pstSess->pstItems);
                        pstSess->pstItems = NULL;
                    } else {
                        TradeItem *pstShrunk = realloc(
                            pstSess->pstItems,
                            (size_t)pstSess->nCount * sizeof(TradeItem));
                        if (NULL != pstShrunk) {
                            pstSess->pstItems = pstShrunk;
                        }
                    }
                }
                printF("Product removed from trade list.\n");
            }
        }
        free(psItem);
        return 0;
    }

    printF("Unknown command. Available: ADD <PRODUCT> <AMOUNT>, "
           "REMOVE <PRODUCT> <AMOUNT>, SEND, CANCEL\n");
    return 0;
}

/********************
 *
 * @Name: trade_session_free
 * @Def: Releases all allocations held by a TradeSession.
 * @Arg: In/Out: pstSess = session to clear
 * @Ret: None
 *
 ********************/
void trade_session_free(TradeSession *pstSess) {
    free(pstSess->pstAllyProducts);
    free_trade_items(pstSess->pstItems, pstSess->nCount);
    memset(pstSess, 0, sizeof(TradeSession));
}

/********************
 *
 * @Name: find_product
 * @Def: Searches a TradeItem list for an exact product name.
 * @Arg: In: pstItems  = selected trade items
 *       In: nCount    = number of selected items
 *       In: psProduct = product name to find
 * @Ret: Index if found, -1 otherwise.
 *
 ********************/
int find_product(TradeItem *pstItems, int nCount, char *psProduct) {
    int i;

    for (i = 0; i < nCount; i++) {
        if (0 == strcmp(pstItems[i].name, psProduct)) {
            return i;
        }
    }
    return -1;
}

/********************
 *
 * @Name: is_number
 * @Def: Checks that a string contains only decimal digits.
 * @Arg: In: psStr = string to validate
 * @Ret: 1 if numeric, 0 otherwise.
 *
 ********************/
int is_number(char *psStr) {
    int i = 0;

    if (NULL == psStr || psStr[0] == '\0') {
        return 0;
    }
    while (psStr[i] != '\0') {
        if (psStr[i] < '0' || psStr[i] > '9') {
            return 0;
        }
        i++;
    }
    return 1;
}

/********************
 *
 * @Name: free_trade_items
 * @Def: Frees a TradeItem array and every duplicated product name inside it.
 * @Arg: In: pstItems = items to free
 *       In: nCount   = number of items
 * @Ret: None
 *
 ********************/
void free_trade_items(TradeItem *pstItems, int nCount) {
    int i;

    if (NULL == pstItems) {
        return;
    }
    for (i = 0; i < nCount; i++) {
        free(pstItems[i].name);
    }
    free(pstItems);
}

/********************
 *
 * @Name: valid_product_for_realm
 * @Def: Checks whether the requested product exists in the cached ally list.
 * @Arg: In: psProduct       = product name requested by user
 *       In: pstAllyProducts = snapshot of ally products
 *       In: nAllyCount      = number of ally products
 * @Ret: 1 if available, 0 otherwise.
 *
 ********************/
static int valid_product_for_realm(char *psProduct,
                                    Product *pstAllyProducts, int nAllyCount) {
    int i;

    for (i = 0; i < nAllyCount; i++) {
        if (0 == strcasecmp(pstAllyProducts[i].name, psProduct)) {
            return 1;
        }
    }
    return 0;
}

/********************
 *
 * @Name: collect_trade_items
 * @Def: Older blocking trade-input collector kept as a helper.
 *       The active terminal path uses TradeSession for non-blocking select().
 * @Arg: In/Out parameters mirror trade_session_init plus output item list.
 * @Ret: 1 when SEND selected, 0 on cancel/error.
 *
 ********************/
int collect_trade_items(char *psRealm, Maester *pstMaester,
                        Product **ppstOwnProducts, int *pnOwnCount,
                        RemoteInventory **ppstCache, int *pnCacheCount,
                        TradeItem **ppstItemsOut, int *pnCountOut) {
    char    *psCommand          = NULL;
    char    *psOutput           = NULL;
    int      nAllyCount         = 0;
    int      nLen;
    int      i;
    Product *pstAllyProducts    = NULL;
    TradeItem *pstSelectedProducts = NULL;
    int        nSelectedCount      = 0;

    (void)pstMaester;
    (void)ppstOwnProducts;
    (void)pnOwnCount;

    *ppstItemsOut = NULL;
    *pnCountOut   = 0;

    /* Snapshot ally products under g_data_mutex so a concurrent LIST PRODUCTS
     * envoy cannot realloc the cache while we're reading it. */
    pthread_mutex_lock(&g_data_mutex);
    {
        Product *pstSrc = ri_get_products(*ppstCache, *pnCacheCount,
                                          psRealm, &nAllyCount);
        if (NULL != pstSrc && nAllyCount > 0) {
            pstAllyProducts = malloc((size_t)nAllyCount * sizeof(Product));
            if (NULL != pstAllyProducts) {
                memcpy(pstAllyProducts, pstSrc,
                       (size_t)nAllyCount * sizeof(Product));
            } else {
                nAllyCount = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_data_mutex);

    if (NULL == pstAllyProducts) {
        printF("No products available. Use LIST PRODUCTS first.\n");
        return 0;
    }

    nLen = asprintf(&psOutput,
        "Trade with %s begins.\n"
        "A direct path is open; your houses are allied, "
        "and no intermediaries stand in between.\n", psRealm);
    if (-1 != nLen) {
        printF(psOutput);
        free(psOutput);
    }

    printF("Available products: ");
    for (i = 0; i < nAllyCount; i++) {
        printF(pstAllyProducts[i].name);
        if (i < nAllyCount - 1) {
            printF(", ");
        }
    }
    printF("\n");

    while (1) {
        printF("(trade)> ");
        psCommand = read_screen();
        if (NULL == psCommand) {
            free(pstAllyProducts);
            free_trade_items(pstSelectedProducts, nSelectedCount);
            return 0;
        }

        to_upper(psCommand);

        {
            int nCmdLen = (int)strlen(psCommand);
            while (nCmdLen > 0 && psCommand[nCmdLen - 1] == '\n') {
                psCommand[nCmdLen - 1] = '\0';
                nCmdLen--;
            }
        }

        if (0 == strcmp(psCommand, "CANCEL")) {
            printF("Trade cancelled.\n");
            free(psCommand);
            free(pstAllyProducts);
            free_trade_items(pstSelectedProducts, nSelectedCount);
            return 0;
        }

        if (0 == strcmp(psCommand, "SEND")) {
            free(psCommand);
            if (0 == nSelectedCount) {
                printF("No items selected. Add items before sending.\n");
                continue;
            }
            free(pstAllyProducts);
            *ppstItemsOut = pstSelectedProducts;
            *pnCountOut   = nSelectedCount;
            return 1;
        }

        {
            char *psW1   = NULL;
            char *psItem = NULL;
            int   nAmount = 0;

            psW1 = strtok(psCommand, " ");

            if (NULL == psW1) {
                printF("Unknown command. Available: ADD <PRODUCT> <AMOUNT>, "
                       "REMOVE <PRODUCT> <AMOUNT>, SEND, CANCEL\n");
                free(psCommand);
                continue;
            }

            if (0 == strcmp(psW1, "ADD") || 0 == strcmp(psW1, "REMOVE")) {
                char *psToks[7];
                int   nNtoks = 0;
                char *psTok  = strtok(NULL, " ");

                while (NULL != psTok && nNtoks < 7) {
                    psToks[nNtoks++] = psTok;
                    psTok = strtok(NULL, " ");
                }

                if (nNtoks < 2 || !is_number(psToks[nNtoks - 1])) {
                    printF("Invalid command. Usage: ADD/REMOVE <PRODUCT> <AMOUNT>\n");
                    free(psCommand);
                    continue;
                }

                nAmount = atoi(psToks[nNtoks - 1]);

                {
                    int    j;
                    size_t nNameLen = 0;

                    for (j = 0; j < nNtoks - 1; j++) {
                        nNameLen += strlen(psToks[j]) + 1;
                    }
                    psItem = malloc(nNameLen);
                    if (NULL == psItem) {
                        free(psCommand);
                        free(pstAllyProducts);
                        free_trade_items(pstSelectedProducts, nSelectedCount);
                        return 0;
                    }
                    psItem[0] = '\0';
                    for (j = 0; j < nNtoks - 1; j++) {
                        if (j > 0) {
                            strcat(psItem, " ");
                        }
                        strcat(psItem, psToks[j]);
                    }
                }

                if (nAmount <= 0) {
                    printF("Invalid amount. Must be a positive integer.\n");
                    free(psItem);
                    free(psCommand);
                    continue;
                }

                if (!valid_product_for_realm(psItem, pstAllyProducts, nAllyCount)) {
                    printF("Product not available from this realm.\n");
                    free(psItem);
                    free(psCommand);
                    continue;
                }

                if (0 == strcmp(psW1, "ADD")) {
                    int nIndex = find_product(pstSelectedProducts,
                                             nSelectedCount, psItem);
                    if (-1 != nIndex) {
                        pstSelectedProducts[nIndex].amount += nAmount;
                    } else {
                        TradeItem *pstNewItems = realloc(
                            pstSelectedProducts,
                            (size_t)(nSelectedCount + 1) * sizeof(TradeItem));
                        if (NULL == pstNewItems) {
                            free(psItem);
                            free(psCommand);
                            free(pstAllyProducts);
                            free_trade_items(pstSelectedProducts, nSelectedCount);
                            return 0;
                        }
                        pstSelectedProducts = pstNewItems;
                        pstSelectedProducts[nSelectedCount].name = strdup(psItem);
                        if (NULL == pstSelectedProducts[nSelectedCount].name) {
                            free(psItem);
                            free(psCommand);
                            free(pstAllyProducts);
                            free_trade_items(pstSelectedProducts, nSelectedCount);
                            return 0;
                        }
                        pstSelectedProducts[nSelectedCount].amount = nAmount;
                        nSelectedCount++;
                    }
                    printF("Product added to trade list.\n");
                } else {
                    int nIndex = find_product(pstSelectedProducts,
                                             nSelectedCount, psItem);
                    if (-1 == nIndex) {
                        printF("Product not in trade list.\n");
                    } else if (pstSelectedProducts[nIndex].amount < nAmount) {
                        printF("Cannot remove more than the current amount.\n");
                    } else {
                        pstSelectedProducts[nIndex].amount -= nAmount;
                        if (0 == pstSelectedProducts[nIndex].amount) {
                            free(pstSelectedProducts[nIndex].name);
                            for (i = nIndex; i < nSelectedCount - 1; i++) {
                                pstSelectedProducts[i] = pstSelectedProducts[i + 1];
                            }
                            nSelectedCount--;
                            if (0 == nSelectedCount) {
                                free(pstSelectedProducts);
                                pstSelectedProducts = NULL;
                            } else {
                                TradeItem *pstShrunk = realloc(
                                    pstSelectedProducts,
                                    (size_t)nSelectedCount * sizeof(TradeItem));
                                if (NULL != pstShrunk) {
                                    pstSelectedProducts = pstShrunk;
                                }
                            }
                        }
                        printF("Product removed from trade list.\n");
                    }
                }
                free(psItem);
                free(psCommand);
                continue;
            }

            printF("Unknown command. Available: ADD <PRODUCT> <AMOUNT>, "
                   "REMOVE <PRODUCT> <AMOUNT>, SEND, CANCEL\n");
            free(psCommand);
        }
    }
}
