CC     = gcc
CFLAGS = -Wall -Wextra -g \
         -iquote terminal -iquote utils -iquote realm \
         -iquote inventory -iquote pledge -iquote network \
         -iquote transfer -iquote handlers -iquote envoy

# ── Object files ────────────────────────────────────────────────────────────
OBJ = maester.o \
      terminal/terminal.o \
      terminal/commands.o \
      utils/io.o \
      inventory/inventory.o \
      pledge/pledge.o \
      network/protocol.o \
      network/network.o \
      transfer/sigil.o \
      transfer/relay.o \
      handlers/message_handler.o \
      handlers/pledge_handler.o \
      handlers/list_handler.o \
      handlers/order_handler.o \
      envoy/envoy.o

# ── Link ─────────────────────────────────────────────────────────────────────
maester: $(OBJ)
	$(CC) $(CFLAGS) -o maester $(OBJ) -lpthread

# ── Compile rules ─────────────────────────────────────────────────────────────
maester.o: maester.c
	$(CC) $(CFLAGS) -c maester.c -o maester.o

terminal/terminal.o: terminal/terminal.c
	$(CC) $(CFLAGS) -c terminal/terminal.c -o terminal/terminal.o

terminal/commands.o: terminal/commands.c
	$(CC) $(CFLAGS) -c terminal/commands.c -o terminal/commands.o

utils/io.o: utils/io.c
	$(CC) $(CFLAGS) -c utils/io.c -o utils/io.o

inventory/inventory.o: inventory/inventory.c
	$(CC) $(CFLAGS) -c inventory/inventory.c -o inventory/inventory.o

pledge/pledge.o: pledge/pledge.c
	$(CC) $(CFLAGS) -c pledge/pledge.c -o pledge/pledge.o

network/protocol.o: network/protocol.c
	$(CC) $(CFLAGS) -c network/protocol.c -o network/protocol.o

network/network.o: network/network.c
	$(CC) $(CFLAGS) -c network/network.c -o network/network.o

transfer/sigil.o: transfer/sigil.c
	$(CC) $(CFLAGS) -c transfer/sigil.c -o transfer/sigil.o

transfer/relay.o: transfer/relay.c
	$(CC) $(CFLAGS) -c transfer/relay.c -o transfer/relay.o

handlers/message_handler.o: handlers/message_handler.c
	$(CC) $(CFLAGS) -c handlers/message_handler.c -o handlers/message_handler.o

handlers/pledge_handler.o: handlers/pledge_handler.c
	$(CC) $(CFLAGS) -c handlers/pledge_handler.c -o handlers/pledge_handler.o

handlers/list_handler.o: handlers/list_handler.c
	$(CC) $(CFLAGS) -c handlers/list_handler.c -o handlers/list_handler.o

handlers/order_handler.o: handlers/order_handler.c
	$(CC) $(CFLAGS) -c handlers/order_handler.c -o handlers/order_handler.o

envoy/envoy.o: envoy/envoy.c
	$(CC) $(CFLAGS) -c envoy/envoy.c -o envoy/envoy.o

# ── Housekeeping ──────────────────────────────────────────────────────────────
clean:
	rm -f maester.o \
	      terminal/*.o \
	      utils/*.o \
	      realm/*.o \
	      inventory/*.o \
	      pledge/*.o \
	      network/*.o \
	      transfer/*.o \
	      handlers/*.o \
	      envoy/*.o \
	      maester

re: clean maester
