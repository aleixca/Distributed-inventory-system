#ifndef __INVENTORY_H__
#define __INVENTORY_H__

/***********************************************
 *
 * @File:    inventory.h
 * @Purpose: Declares Product, RemoteInventory, and TradeItem structures plus all inventory management functions.
 * @Author:  Group 6
 * @Date:    2026-05-15
 *
 ***********************************************/
#define _GNU_SOURCE

#include "io.h"
#include "maester_types.h"
/* REALM_NAME_SIZE is used by RemoteInventory. */
#include "pledge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Product ─────────────────────────────────────────────────────────────── */

typedef struct {
    /* Product name stored in stock.db. */
    char  name[100];
    /* Available units. */
    int   amount;
    /* Product weight/value field from statement. */
    float weight;
} Product;

Product *load_inventory(char *psPath, int *pnTotalProducts);
void     list_products(int nTotalProducts, Product *pstProducts);
void     free_inventory(Product *pstProducts);

/* ── Stock file operations ───────────────────────────────────────────────── */

int write_stock_file(char *psPath, Product *pstProducts, int nCount);
int apply_stock_delta(Product *pstProducts, int nCount, char *psName, int nDelta);
int add_or_update_product(Product **ppstProducts, int *pnCount,
                           const char *psName, int nAmount, float fWeight);

/* ── Remote inventory cache ──────────────────────────────────────────────── */

typedef struct {
    /* Remote realm key. */
    char     realm_name[REALM_NAME_SIZE];
    /* Cached Product[] snapshot. */
    Product *products;
    /* Number of cached products. */
    int      count;
} RemoteInventory;

RemoteInventory *ri_set_products(RemoteInventory *pstCache, int *pnCacheCount,
                                  char *psRealm, Product *pstProducts, int nCount);
Product *ri_get_products(RemoteInventory *pstCache, int nCacheCount,
                          char *psRealm, int *pnCountOut);
void     ri_free_all(RemoteInventory *pstCache, int nCacheCount);

/* ── Trade ───────────────────────────────────────────────────────────────── */

typedef struct {
    /* Heap-owned product name selected for trade. */
    char *name;
    /* Requested units. */
    int   amount;
} TradeItem;

int  collect_trade_items(char *psRealm, Maester *pstMaester,
                          Product **ppstOwnProducts, int *pnOwnCount,
                          RemoteInventory **ppstCache, int *pnCacheCount,
                          TradeItem **ppstItemsOut, int *pnCountOut);
int  find_product(TradeItem *pstItems, int nCount, char *psProduct);
int  is_number(char *psStr);
void free_trade_items(TradeItem *pstItems, int nCount);

/* ── Non-blocking trade session (used by the select() loop) ─────────────── */

typedef struct {
    /* Ally being traded with. */
    char       sRealm[50];
    /* Snapshot shown during trade. */
    Product   *pstAllyProducts;
    /* Products in snapshot. */
    int        nAllyCount;
    /* Selected ADD items. */
    TradeItem *pstItems;
    /* Selected item count. */
    int        nCount;
} TradeSession;

/* Snapshot ally products and print the opening banner.
 * Returns 0 on success, -1 if no cached products (user must LIST PRODUCTS first). */
int  trade_session_init(TradeSession *pstSess, char *psRealm,
                        RemoteInventory **ppstCache, int *pnCacheCount);

/* Process one line of already-uppercased trade input (ADD/REMOVE/SEND/CANCEL).
 * Returns: 0 = continue, 1 = SEND (items ready in pstSess), -1 = cancelled. */
int  trade_session_input(TradeSession *pstSess, char *psLine);

/* Free all resources held by the session. */
void trade_session_free(TradeSession *pstSess);

#endif
