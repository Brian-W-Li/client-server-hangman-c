#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ---------- utilities ----------

// recv exactly len bytes (unless error/EOF). 0 on success, -1 on error/EOF.
static int recv_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

// send all bytes
static int send_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(fd, buf + total, len - total, 0);
        if (s < 0) {
            return -1;
        }
        total += (size_t)s;
    }
    return 0;
}

/*
 * Receive exactly one server packet (message or game-control), print it,
 * and return:
 *   1 = "server-overloaded" message
 *   2 = "Game Over!" message
 *   3 = game-control packet (board update)
 *   4 = other message ("Welcome...", "The word was...", "You Win!", "You Lose.")
 *  -1 = error
 */
static int recv_and_print_one_packet(int sockfd) {
    unsigned char msg_flag;

    // Read first byte: msg_flag
    if (recv_all(sockfd, &msg_flag, 1) < 0) {
        fprintf(stderr, "Error: failed to read msg_flag from server\n");
        return -1;
    }

    if (msg_flag > 0) {
        // Message packet
        unsigned char data[256];

        if (recv_all(sockfd, data, msg_flag) < 0) {
            fprintf(stderr, "Error: failed to read message data\n");
            return -1;
        }

        const char *over = "server-overloaded";
        size_t over_len  = strlen(over);

        const char *game_over = "Game Over!";
        size_t game_over_len  = strlen(game_over);

        if (msg_flag == over_len && memcmp(data, over, over_len) == 0) {
            printf(">>>%.*s\n", (int)msg_flag, (char *)data);
            return 1;  // overloaded
        }

        if (msg_flag == game_over_len && memcmp(data, game_over, game_over_len) == 0) {
            printf(">>>%.*s\n", (int)msg_flag, (char *)data);
            return 2;  // explicit end of game
        }

        // Normal message (welcome, "The word was ...", "You Win!", "You Lose.")
        printf(">>>%.*s\n", (int)msg_flag, (char *)data);
        return 4;
    } else {
        // Game-control packet
        unsigned char header[2];  // word_length, num_incorrect
        if (recv_all(sockfd, header, 2) < 0) {
            fprintf(stderr, "Error: failed to read game-control header\n");
            return -1;
        }

        unsigned char word_len      = header[0];
        unsigned char num_incorrect = header[1];

        if (word_len > 8) {
            fprintf(stderr, "Error: invalid word length from server\n");
            return -1;
        }

        unsigned int data_len = (unsigned int)word_len + (unsigned int)num_incorrect;
        unsigned char data[16];  // enough for 8 + 8
        if (data_len > sizeof(data)) {
            fprintf(stderr, "Error: game-control data too long\n");
            return -1;
        }

        if (recv_all(sockfd, data, data_len) < 0) {
            fprintf(stderr, "Error: failed to read game-control data\n");
            return -1;
        }

        unsigned char *word_state = data;
        unsigned char *incorrect  = data + word_len;

        // Print masked word like: >>>_ _ _
        printf(">>>");
        for (unsigned char i = 0; i < word_len; i++) {
            printf("%c", word_state[i]);
            if (i + 1 < word_len) {
                printf(" ");
            }
        }
        printf("\n");

        // Print incorrect guesses line
        printf(">>>Incorrect Guesses:");
        if (num_incorrect > 0) {
            printf(" ");
            for (unsigned char j = 0; j < num_incorrect; j++) {
                printf("%c", incorrect[j]);
                if (j + 1 < num_incorrect) {
                    printf(" ");
                }
            }
        }
        printf("\n");

        // Blank line with >>>
        printf(">>>\n");

        return 3;  // game-control
    }
}

// ---------- main ----------

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int server_port = atoi(argv[2]);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    // First packet is either "server-overloaded" or "Welcome to Hangman"
    int r = recv_and_print_one_packet(sockfd);
    if (r < 0) {
        close(sockfd);
        return 1;
    }
    if (r == 1) {
        // overloaded; do not prompt
        close(sockfd);
        return 0;
    }
    if (r == 2) {
        // weird, but if server sent Game Over immediately, we're done
        close(sockfd);
        return 0;
    }

    // Accepted. Ask user if they want to start.
    char line[128];
    printf(">>> Ready to start game? (y/n): ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin)) {
        close(sockfd);
        return 1;
    }

    if (line[0] != 'y' && line[0] != 'Y') {
        close(sockfd);
        return 0;
    }

    // Send empty start message: [msg_len = 0]
    unsigned char msg_len = 0;
    if (send_all(sockfd, (char *)&msg_len, 1) < 0) {
        perror("send start");
        close(sockfd);
        return 1;
    }

    // Receive initial game-control packet and any messages before it.
    for (;;) {
        r = recv_and_print_one_packet(sockfd);
        if (r < 0) {
            close(sockfd);
            return 1;
        }
        if (r == 2) {
            // Game Over before we even get a board
            close(sockfd);
            return 0;
        }
        if (r == 3) {
            // got initial board
            break;
        }
        // r == 4 => some message; keep reading until we get board or Game Over
    }

    int game_over = 0;

    // Guess loop: blank line => quit (even if game not finished).
    while (!game_over) {
        printf(">>>Letter to guess: ");
        fflush(stdout);

        // Read user input
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        // Strip newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        // blank line => quit
        if (len == 0) {
            break;
        }

        // must be *exactly* one alphabetic char
        if (len != 1 || !isalpha((unsigned char)line[0])) {
            printf(">>>Error! Please guess one letter.\n");
            continue;
        }

        char guess = (char)tolower((unsigned char)line[0]);

        // Send guess packet: [1-byte length=1][1-byte letter]
        unsigned char len_byte = 1;
        unsigned char payload  = (unsigned char)guess;

        if (send_all(sockfd, (char *)&len_byte, 1) < 0) {
            perror("send guess header");
            break;
        }
        if (send_all(sockfd, (char *)&payload, 1) < 0) {
            perror("send guess payload");
            break;
        }

        // After a guess, server may send:
        //  - just one game-control packet (continue)
        //  - OR: "The word was...", "You Win!/You Lose.", "Game Over!"
        // We keep reading until:
        //   - Game Over (2)    => set game_over, break outer
        //   - Board (3)        => break inner, ask for next guess
        while (1) {
            r = recv_and_print_one_packet(sockfd);
            if (r < 0) {
                game_over = 1;
                break;
            }
            if (r == 2) {
                // "Game Over!"
                game_over = 1;
                break;
            }
            if (r == 3) {
                // got a fresh board; continue guessing
                break;
            }
            // r == 4: some message ("The word was...", "You Win!", ...).
            // Just printed it; keep reading until board or Game Over.
        }
    }

    close(sockfd);
    return 0;
}