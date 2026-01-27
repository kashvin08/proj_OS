CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread -Isrc/include

all: server client

server: src/server.c src/game.c
	$(CC) $(CFLAGS) -o server src/server.c src/game.c src/state.c

client: src/client.c
	$(CC) $(CFLAGS) -o client src/client.c

clean:
	rm -f server client
