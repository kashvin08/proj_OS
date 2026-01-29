#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "include/protocol.h"

static int safe_mkfifo(const char *path) {
    unlink(path);
    if (mkfifo(path, 0666) == -1) {
        perror("mkfifo");
        return -1;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r == 0)
            return 0;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 1;
}

int main(void) {
    char name[MAX_NAME];

    printf("Enter your name: ");
    fflush(stdout);
    if (!fgets(name, sizeof(name), stdin))
        return 1;
    name[strcspn(name, "\n")] = 0;

    pid_t pid = getpid();

    char reply_fifo[MAX_PATH];
    snprintf(reply_fifo, sizeof(reply_fifo), "/tmp/race30_reply_%d.fifo", (int)pid);

    if (safe_mkfifo(reply_fifo) != 0)
        return 1;

    // Open reply fifo RDWR so the open doesn't block
    int reply_fd = open(reply_fifo, O_RDWR);
    if (reply_fd < 0) {
        perror("open reply fifo");
        unlink(reply_fifo);
        return 1;
    }

    // Send JOIN to lobby
    int lobby_fd = open(LOBBY_FIFO, O_WRONLY);
    if (lobby_fd < 0) {
        perror("open lobby fifo");
        fprintf(stderr, "Is the server running?\n");
        close(reply_fd);
        unlink(reply_fifo);
        return 1;
    }

    ClientMsg join = {0};
    join.type = MSG_JOIN;
    join.pid = pid;
    snprintf(join.name, sizeof(join.name), "%s", name);
    snprintf(join.reply_fifo, sizeof(join.reply_fifo), "%s", reply_fifo);
    
    if (write_all(lobby_fd, &join, sizeof(join)) != 0) {
        perror("write JOIN");
        close(lobby_fd);
        close(reply_fd);
        unlink(reply_fifo);
        return 1;
    }
    close(lobby_fd);

    // Read JOIN response
    ServerMsg resp = {0};
    if (read_all(reply_fd, &resp, sizeof(resp)) <= 0) {
        fprintf(stderr, "Failed to read JOIN response.\n");
        close(reply_fd);
        unlink(reply_fifo);
        return 1;
    }

    printf("[SERVER] %s\n", resp.text);
    if (!resp.ok || resp.assigned_req_fifo[0] == '\0') {
        close(reply_fd);
        unlink(reply_fifo);
        return 1;
    }

    // Open assigned request fifo for writing
    int req_fd = open(resp.assigned_req_fifo, O_WRONLY);
    if (req_fd < 0) {
        perror("open assigned request fifo");
        close(reply_fd);
        unlink(reply_fifo);
        return 1;
    }

    printf("Connected! Type a move (1-3) or 'q' to quit.\n");
    printf("Game uses round-robin: players take turns with 30-second timeouts.\n");

    while (1) {
        char line[64];
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;
        line[strcspn(line, "\n")] = 0;

        ClientMsg msg = {0};
        msg.pid = pid;

        if (strcmp(line, "q") == 0 || strcmp(line, "quit") == 0) {
            msg.type = MSG_QUIT;
            (void)write_all(req_fd, &msg, sizeof(msg));

            if (read_all(reply_fd, &resp, sizeof(resp)) > 0) {
                printf("[SERVER] %s\n", resp.text);
            }
            break;
        }

        msg.type = MSG_MOVE;
        char *end = NULL;
        long v = strtol(line, &end, 10);
        if (end == line || *end != '\0') {
            printf("Enter 1-3 or q.\n");
            continue;
        }
        msg.move = (int)v;

        if (write_all(req_fd, &msg, sizeof(msg)) != 0) {
            perror("write MOVE");
            break;
        }

        if (read_all(reply_fd, &resp, sizeof(resp)) <= 0) {
            fprintf(stderr, "Server disconnected.\n");
            break;
        }

        printf("[SERVER] %s\n", resp.text);
    }

    close(req_fd);
    close(reply_fd);
    unlink(reply_fifo);
    return 0;
}
