# Distributed Inventory System

A distributed inventory and message-passing system written in C. The project simulates a network of independent realms that communicate through TCP sockets, exchange alliance requests, query remote inventories, and coordinate stock transfers through a custom fixed-size frame protocol.

The system is built around one executable, `maester`. Each running Maester owns one realm, listens on its configured IP and port, keeps a local binary stock database, and uses child processes called Envoys to perform outgoing network missions without blocking the interactive terminal.

## Features

- TCP server/client communication between independent realm processes
- Custom 320-byte binary protocol with message types, payload length, and checksum validation
- Static routing table with direct routes and `DEFAULT` fallback routes for multi-hop communication
- Interactive terminal for local and remote inventory operations
- Alliance workflow using sigil file transfer and MD5 verification
- Remote product listing with local caching
- Trade workflow that updates both in-memory inventory and the binary stock database
- Concurrent incoming request handling with detached POSIX threads
- Outgoing task isolation through Envoy child processes
- Graceful shutdown with disconnect notifications

## Architecture

```text
.
├── maester.c              # Entry point, config loading, lifecycle
├── terminal/              # Interactive command parser and event loop
├── network/               # TCP sockets, routing, frame send/receive
├── handlers/              # Incoming frame dispatch and protocol handlers
├── envoy/                 # Child-process task execution
├── inventory/             # Stock loading, listing, caching, trade updates
├── pledge/                # Alliance state and pledge lifecycle
├── transfer/              # Sigil/file transfer, relay, MD5 utilities
├── realm/                 # Shared Maester and route types
├── utils/                 # Low-level I/O helpers
└── realms/                # Example realm configs, sigils, and stock DBs
```

At runtime, each Maester multiplexes stdin, network sockets, Envoy pipes, and async notifications with `select()`. Incoming connections are handled in threads so file transfers and remote requests do not freeze the terminal. Outgoing missions run in Envoy child processes so the parent can keep accepting commands.

## Requirements

Use a POSIX-like environment such as Linux, WSL, or a Unix lab machine.

Required tools:

- `gcc`
- `make`
- POSIX sockets and pthreads
- `md5sum`

On Ubuntu or WSL:

```bash
sudo apt update
sudo apt install build-essential coreutils
```

## Build

From the repository root:

```bash
make
```

This creates the `maester` executable.

To clean generated object files and the executable:

```bash
make clean
```

To rebuild from scratch:

```bash
make re
```

## Run

The executable expects two arguments:

```bash
./maester <maester_config.txt> <stock_database.db>
```

Example:

```bash
./maester realms/baratheon/baratheon.txt realms/baratheon/baratheon_stock.db
```

To test the distributed behavior, start multiple Maesters in separate terminals:

```bash
./maester realms/arryn/arryn.txt realms/arryn/arryn_stock.db
./maester realms/baratheon/baratheon.txt realms/baratheon/baratheon_stock.db
./maester realms/cole/cole.txt realms/cole/cole_stock.db
./maester realms/dustin/dustin.txt realms/dustin/dustin_stock.db
./maester realms/greyjoy/greyjoy.txt realms/greyjoy/greyjoy_stock.db
```

For local testing, use 127.0.0.1 with different ports for each realm.
For LAN testing, replace 127.0.0.1 with each machine's private LAN IP.

## Configuration Format

Each realm config follows this format:

```text
<realm name>
<realm directory>
<envoy count>
<listen ip>
<listen port>
--- ROUTES ---
<destination realm> <next-hop ip> <next-hop port>
DEFAULT <next-hop ip> <next-hop port>
```

Example:

```text
Baratheon
realms/baratheon
3
192.168.1.4
8821
--- ROUTES ---
Arryn 192.168.1.3 8820
Cole 192.168.1.4 8822
Dustin 192.168.1.5 8823
DEFAULT 192.168.1.5 8823
```

The second CLI argument is the realm stock database. It is a binary file containing consecutive `Product` records:

```c
typedef struct {
    char  name[100];
    int   amount;
    float weight;
} Product;
```

## Terminal Commands

Run these commands inside a Maester prompt.

```text
LIST REALMS
```

Lists the realms known through the local routing table.

```text
LIST PRODUCTS
```

Lists the local realm inventory.

```text
PLEDGE <REALM> <SIGIL_PATH>
```

Sends an alliance request to another realm and transfers a sigil file.

Example:

```text
PLEDGE Baratheon realms/arryn/arryn.png
```

The sigil must be inside the sender's own realm directory.

```text
PLEDGE RESPOND <REALM> ACCEPT
PLEDGE RESPOND <REALM> REJECT
```

Accepts or rejects a pending pledge request.

```text
PLEDGE STATUS
```

Shows pending, accepted, and rejected pledge states.

```text
LIST PRODUCTS <REALM>
```

Requests a remote ally's product list and caches it locally.

```text
START TRADE <REALM>
```

Starts a trade session with an allied realm. You must run `LIST PRODUCTS <REALM>` first so the remote inventory is cached.

Inside trade mode:

```text
ADD <PRODUCT> <AMOUNT>
REMOVE <PRODUCT> <AMOUNT>
SEND
CANCEL
```

```text
ENVOY STATUS
```

Shows the status of outgoing Envoy slots.

```text
EXIT
```

Gracefully shuts down the Maester and notifies allied realms.

## Manual Test Checklist

There is no separate automated test suite in this repository. The project is tested by running multiple Maester processes and exercising the protocol flows below.

### 1. Build Test

```bash
make clean
make
```

Expected result: the build finishes and creates `./maester`.

### 2. Startup Test

Open two terminals and start two directly connected realms:

```bash
./maester realms/arryn/arryn.txt realms/arryn/arryn_stock.db
```

```bash
./maester realms/baratheon/baratheon.txt realms/baratheon/baratheon_stock.db
```

Expected result: both processes show the `$` prompt and remain running.

If startup fails with a bind or network error, check that the IP in the config exists on your machine. For local-only testing, use `127.0.0.1` in copied config files.

### 3. Local Command Test

In either terminal:

```text
LIST REALMS
LIST PRODUCTS
ENVOY STATUS
```

Expected result: routes, local stock, and Envoy states are printed.

### 4. Pledge Test

From Arryn:

```text
PLEDGE Baratheon realms/arryn/arryn.png
```

From Baratheon:

```text
PLEDGE STATUS
PLEDGE RESPOND Arryn ACCEPT
```

From Arryn:

```text
PLEDGE STATUS
```

Expected result: the pledge moves from pending to accepted/allied.

### 5. Remote Product List Test

After the pledge is accepted, from Arryn:

```text
LIST PRODUCTS Baratheon
```

Expected result: Arryn receives and displays Baratheon's inventory.

### 6. Trade Test

After listing Baratheon's products from Arryn:

```text
START TRADE Baratheon
```

Then, inside the `(trade)>` prompt:

```text
ADD <PRODUCT_NAME> 1
SEND
```

Expected result: the trade request is sent, inventories are updated, and the stock database is rewritten.

Use a product name exactly as it appears in the remote product list.

### 7. Trade Cancel Test

```text
START TRADE Baratheon
ADD <PRODUCT_NAME> 1
REMOVE <PRODUCT_NAME> 1
CANCEL
```

Expected result: the session exits without sending an order.

### 8. Multi-Hop Routing Test

Start the full example network in separate terminals:

```bash
./maester realms/arryn/arryn.txt realms/arryn/arryn_stock.db
./maester realms/baratheon/baratheon.txt realms/baratheon/baratheon_stock.db
./maester realms/dustin/dustin.txt realms/dustin/dustin_stock.db
./maester realms/greyjoy/greyjoy.txt realms/greyjoy/greyjoy_stock.db
```

Send a pledge or list request between non-neighbor realms that require a `DEFAULT` route.

Expected result: intermediate Maesters relay the frame toward the destination, or the origin receives an unknown-realm/no-route response.

### 9. Error Handling Test

Try invalid commands:

```text
LIST
PLEDGE
START TRADE
PLEDGE UnknownRealm realms/arryn/arryn.png
```

Expected result: the program prints clear error messages and keeps running.

### 10. Shutdown Test

In each running terminal:

```text
EXIT
```

Expected result: each Maester signs off, closes sockets, frees inventory/config memory, and broadcasts disconnect messages to allies.

## Notes

- The stock databases are binary files, not text files.
- Trades mutate the relevant stock database, so copy the `.db` files before testing if you want a clean baseline.
- For reliable local testing, keep all realm ports unique.
- Firewall rules can block cross-machine testing; allow inbound TCP on the configured ports.
- `CTRL+C` is handled as a graceful shutdown path, but `EXIT` is the preferred way to close a Maester during tests.
