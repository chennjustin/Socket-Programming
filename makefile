CC = gcc
CFLAGS = -Wall -O2
LIBS = -lssl -lcrypto -lpthread

all: server client

server: server.c
	$(CC) $(CFLAGS) server.c -o server $(LIBS)

client: client.c
	$(CC) $(CFLAGS) client.c -o client $(LIBS)

clean:
	rm -f server client
