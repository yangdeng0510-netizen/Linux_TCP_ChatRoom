CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -pthread

all: client

client: client.o
	$(CC) -o client client.o $(LDFLAGS)

client.o: client.c
	$(CC) $(CFLAGS) -c client.c

clean:
	rm -f *.o client
