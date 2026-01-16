CC = gcc
CFLAGS = -Wall -g -pthread
# -lrt is for POSIX Shared Memory, -lpthread is for threads
LIBS = -lrt -lpthread
SRC_DIR = src

# Targets
all: server client

# Server now includes logger and persistence logic
server: $(SRC_DIR)/server.c $(SRC_DIR)/persistence.c $(SRC_DIR)/logger.c $(SRC_DIR)/common.h
	$(CC) $(CFLAGS) -o server $(SRC_DIR)/server.c $(SRC_DIR)/persistence.c $(SRC_DIR)/logger.c $(LIBS)

client: $(SRC_DIR)/client.c $(SRC_DIR)/common.h
	$(CC) $(CFLAGS) -o client $(SRC_DIR)/client.c $(LIBS)

clean:
	rm -f server client
	# Note: We usually don't delete scores.txt in 'clean' so we don't lose data, 
	# but we can delete the log and the shared memory file.
	rm -f data/game.log /dev/shm/game_shm