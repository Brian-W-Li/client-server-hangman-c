CC = gcc
CFLAGS = -Wall -Wextra -g

CLIENT = hangman_client
SERVER = hangman_server

all: $(CLIENT) $(SERVER)

$(CLIENT): hangman_client.c
	$(CC) $(CFLAGS) -o $(CLIENT) hangman_client.c

$(SERVER): hangman_server.c
	$(CC) $(CFLAGS) -o $(SERVER) hangman_server.c

clean:
	rm -f $(CLIENT) $(SERVER)
	rm -rf $(CLIENT).dSYM $(SERVER).dSYM