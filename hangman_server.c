#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>

#define MAX_CLIENTS   3
#define BACKLOG       16
#define MAX_WORDS     1024
#define MAX_WORD_LEN  16   // per spec
#define MAX_INCORRECT 8

static char words[MAX_WORDS][MAX_WORD_LEN + 1]; // +1 for '\0'
static int  num_words = 0;

// ---------- utilities ----------

// recv exactly len bytes (unless error/EOF). 0 on success, -1 on error.
static int recv_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    unsigned char *p = (unsigned char *)buf;
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

// send all bytes in buf over TCP (handles partial sends)
static int send_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(fd, buf + total, len - total, 0);
        if (sent < 0) {
            return -1;
        }
        total += (size_t)sent;
    }
    return 0;
}

// load word list from hangman_words.txt
static void load_words(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("fopen hangman_words.txt");
        exit(1);
    }

    char line[256];
    while (num_words < MAX_WORDS && fgets(line, sizeof(line), f)) {
        // strip newline
        char *p = line;
        while (*p && *p != '\n' && *p != '\r') p++;
        *p = '\0';

        size_t len = strlen(line);
        if (len == 0) continue;
        if (len > MAX_WORD_LEN) continue;

        int ok = 1;
        for (size_t i = 0; i < len; i++) {
            if (!isalpha((unsigned char)line[i])) {
                ok = 0;
                break;
            }
        }
        if (!ok) continue;

        // store lowercase version
        for (size_t i = 0; i < len; i++) {
            words[num_words][i] = (char)tolower((unsigned char)line[i]);
        }
        words[num_words][len] = '\0';
        num_words++;
    }

    fclose(f);

    if (num_words == 0) {
        fprintf(stderr, "No valid words loaded from %s\n", filename);
        exit(1);
    }
}

// Send a message packet: msg_flag = length, then that many bytes.
static int send_message_packet(int fd, const char *msg) {
    size_t len = strlen(msg);
    if (len > 255) len = 255;  // protocol uses 1-byte length

    unsigned char header = (unsigned char)len;
    if (send_all(fd, (char *)&header, 1) < 0) return -1;
    if (len > 0 && send_all(fd, msg, len) < 0) return -1;

    return 0;
}

// Send current game-control state for this client:
// msg_flag = 0
// [0] = 0
// [1] = word_len
// [2] = num_incorrect
// then: word_len bytes of masked word
// then: num_incorrect bytes of incorrect letters
static int send_game_state(int client_fd,
                           const char *masked,
                           const unsigned char *incorrect,
                           unsigned char word_len,
                           unsigned char num_incorrect)
{
    if (word_len == 0 || word_len > MAX_WORD_LEN) return -1;

    unsigned char header[3];
    header[0] = 0;              // msg_flag = 0 => game-control
    header[1] = word_len;
    header[2] = num_incorrect;

    unsigned char data[MAX_WORD_LEN + MAX_WORD_LEN]; // 8 + 8 max
    if ((int)word_len + (int)num_incorrect > (int)sizeof(data)) {
        return -1;
    }

    // copy masked
    for (unsigned char i = 0; i < word_len; i++) {
        data[i] = (unsigned char)masked[i];
    }
    // copy incorrect
    for (unsigned char j = 0; j < num_incorrect; j++) {
        data[word_len + j] = incorrect[j];
    }

    if (send_all(client_fd, (char *)header, sizeof(header)) < 0) return -1;
    if (send_all(client_fd, (char *)data, word_len + num_incorrect) < 0) return -1;

    return 0;
}

// ---------- per-client handler (child) ----------

static void handle_client(int client_fd) {
    ssize_t n;
    uint8_t msg_len;

    // 0) Send a welcome message packet immediately.
    //    Client prints this as ">>>Welcome to Hangman"
    if (send_message_packet(client_fd, "Welcome to Hangman") < 0) {
        return;
    }

    // 1) Read the one-byte "start game" header from client (msg_len=0).
    n = recv(client_fd, &msg_len, 1, 0);
    if (n <= 0) {
        // client closed or error before starting
        return;
    }

    // seed RNG uniquely per child
    unsigned int seed = (unsigned int)(time(NULL) ^ (getpid() << 16));
    srand(seed);

    // 2) Choose a random word for this client and initialize state.
    int idx = rand() % num_words;
    const char *secret = words[idx];

    unsigned char word_len = (unsigned char)strlen(secret);

    char masked[MAX_WORD_LEN];
    for (unsigned char i = 0; i < word_len; i++) {
        masked[i] = '_';
    }

    unsigned char incorrect[MAX_INCORRECT];   // allow up to 8 incorrect
    unsigned char num_incorrect = 0;

    // send initial board
    if (send_game_state(client_fd, masked, incorrect, word_len, num_incorrect) < 0) {
        perror("send_game_state");
        return;
    }

    // 3) Guess loop.
    for (;;) {
        uint8_t guess_len;

        n = recv(client_fd, &guess_len, 1, 0);
        if (n <= 0) {
            // client closed or error
            break;
        }

        if (guess_len != 1) {
            // invalid guess packet, drain and ignore
            char tmp[256];
            size_t remaining = guess_len;
            while (remaining > 0) {
                size_t chunk = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
                if (recv_all(client_fd, tmp, chunk) < 0) {
                    remaining = 0;
                    break;
                }
                remaining -= chunk;
            }
            continue;
        }

        unsigned char letter;
        if (recv_all(client_fd, &letter, 1) < 0) {
            break;
        }

        letter = (unsigned char)tolower(letter);

        // Check if this letter was already guessed (in masked or incorrect)
        int already_guessed = 0;
        for (unsigned char i = 0; i < word_len; i++) {
            if (masked[i] == (char)letter) {
                already_guessed = 1;
                break;
            }
        }
        if (!already_guessed) {
            for (unsigned char j = 0; j < num_incorrect; j++) {
                if (incorrect[j] == letter) {
                    already_guessed = 1;
                    break;
                }
            }
        }

        if (!already_guessed) {
            // First time seeing this letter. Check if it's in the secret word.
            int found = 0;
            for (unsigned char i = 0; i < word_len; i++) {
                if ((unsigned char)secret[i] == letter) {
                    masked[i] = (char)letter;
                    found = 1;
                }
            }

            if (!found) {
                if (num_incorrect < MAX_INCORRECT) {
                    incorrect[num_incorrect++] = letter;
                }
            }
        }

        // Check for win
        int all_revealed = 1;
        for (unsigned char i = 0; i < word_len; i++) {
            if (masked[i] == '_') {
                all_revealed = 0;
                break;
            }
        }

        if (all_revealed) {
            // send final board one last time if you want (optional),
            // but spec only cares about the win/lose messages.
            // Send:
            //   "The word was l o o k"
            //   "You Win!"
            //   "Game Over!"
            char word_msg[3 * MAX_WORD_LEN + 32];
            int pos = 0;
            pos += snprintf(word_msg + pos, sizeof(word_msg) - pos,
                            "The word was");
            for (unsigned char i = 0; i < word_len; i++) {
                pos += snprintf(word_msg + pos, sizeof(word_msg) - pos,
                                " %c", secret[i]);
            }
            word_msg[sizeof(word_msg) - 1] = '\0';

            (void)send_message_packet(client_fd, word_msg);
            (void)send_message_packet(client_fd, "You Win!");
            (void)send_message_packet(client_fd, "Game Over!");
            break;
        }

        // Check for lose (>= 6 incorrect guesses)
        if (num_incorrect >= MAX_INCORRECT) {
            char word_msg[3 * MAX_WORD_LEN + 32];
            int pos = 0;
            pos += snprintf(word_msg + pos, sizeof(word_msg) - pos,
                            "The word was");
            for (unsigned char i = 0; i < word_len; i++) {
                pos += snprintf(word_msg + pos, sizeof(word_msg) - pos,
                                " %c", secret[i]);
            }
            word_msg[sizeof(word_msg) - 1] = '\0';

            (void)send_message_packet(client_fd, word_msg);
            (void)send_message_packet(client_fd, "You Lose.");
            (void)send_message_packet(client_fd, "Game Over!");
            break;
        }

        // Otherwise, send updated board
        if (send_game_state(client_fd, masked, incorrect, word_len, num_incorrect) < 0) {
            perror("send_game_state");
            break;
        }
    }
}

// ---------- main server loop ----------

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    if (setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt");
        close(lsock);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(lsock);
        return 1;
    }

    if (listen(lsock, BACKLOG) < 0) {
        perror("listen");
        close(lsock);
        return 1;
    }

    int active_clients = 0;
    printf("Hangman server listening on port %d\n", port);

    load_words("hangman_words.txt");
    printf("Loaded %d words from hangman_words.txt\n", num_words);

    for (;;) {
        int status;
        pid_t pid;

        // Reap finished children BEFORE accept()
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (active_clients > 0) {
                active_clients--;
                printf("Client exited, active_clients = %d\n", active_clients);
            }
        }

        int client_fd = accept(lsock, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        // Reap children that might have finished while we were blocked in accept()
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (active_clients > 0) {
                active_clients--;
                printf("Client exited, active_clients = %d\n", active_clients);
            }
        }

        // Enforce MAX_CLIENTS with "server-overloaded" message packet
        if (active_clients >= MAX_CLIENTS) {
            (void)send_message_packet(client_fd, "server-overloaded");
            close(client_fd);
            printf("Rejected client (server busy). active_clients = %d\n", active_clients);
            continue;
        }

        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }

        if (child == 0) {
            // Child
            close(lsock);
            handle_client(client_fd);
            close(client_fd);
            _exit(0);
        } else {
            // Parent
            close(client_fd);
            active_clients++;
            printf("Accepted new client, active_clients = %d\n", active_clients);
        }
    }

    close(lsock);
    return 0;
}
