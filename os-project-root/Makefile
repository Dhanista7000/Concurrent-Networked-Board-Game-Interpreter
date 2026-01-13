CC = gcc
CFLAGS = -Wall -g -pthread
LIBS = -lrt
SRC_DIR = src

# Targets
all: server client

server: $(SRC_DIR)/server.c $(SRC_DIR)/common.h
	$(CC) $(CFLAGS) -o server $(SRC_DIR)/server.c $(LIBS)

client: $(SRC_DIR)/client.c $(SRC_DIR)/common.h
	$(CC) $(CFLAGS) -o client $(SRC_DIR)/client.c

clean:
	rm -f server client game.log scores.txt