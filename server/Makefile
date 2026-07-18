CC = gcc
CFLAGS = -Wall -DSQLITE_THREADSAFE
LIBS = -lpthread -lsqlite3
TARGET = chat_server
SRCS = main.c net.c db.c client.c handle.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJS) $(TARGET) $(DB_FILE)

run:all
	./$(TARGET)
