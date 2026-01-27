#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "include/protocol.h"
#include "include/game.h"
#include <semaphore.h>
#include "include/state.h"

static GameState *g_state = NULL;
static sem_t *g_sem = NULL;

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_sigchld(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
    {
    }
}

static int safe_mkfifo(const char *path)
{
    unlink(path);
    if (mkfifo(path, 0666) == -1)
    {
        perror("mkfifo");
        return -1;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n)
    {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    size_t off = 0;
    while (off < n)
    {
        ssize_t r = read(fd, p + off, n - off);
        if (r == 0)
            return 0;
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 1;
}

static void child_session_loop(const char *req_fifo, const char *reply_fifo, const char *player_name)
{
    int req_fd = open(req_fifo, O_RDONLY);
    if (req_fd < 0)
    {
        perror("[CHILD] open req fifo");
        _exit(1);
    }

    int rep_fd = open(reply_fifo, O_WRONLY);
    if (rep_fd < 0)
    {
        perror("[CHILD] open reply fifo");
        close(req_fd);
        _exit(1);
    }

    ServerMsg out = {0};
    // out.ok = 1;
    // sem_wait(g_sem);
    // int snapshot = g_state->total;
    // sem_post(g_sem);

    // snprintf(out.text, sizeof(out.text),
    //          "Hi %s! Session started. Shared total=%d. Send MOVE 1-3 or QUIT.",
    //          player_name, snapshot);

    // write_all(rep_fd, &out, sizeof(out));

    while (1)
    {
        ClientMsg in = {0};
        int rr = read_all(req_fd, &in, sizeof(in));
        if (rr <= 0)
            break;

        memset(&out, 0, sizeof(out));

        if (in.type == MSG_MOVE)
        {
            char err[128];

            sem_wait(g_sem);

            if (!validate_move(g_state->total, in.move, err, sizeof(err)))
            {
                sem_post(g_sem);
                out.ok = 0;
                snprintf(out.text, sizeof(out.text), "%s", err);
                write_all(rep_fd, &out, sizeof(out));
                continue;
            }

            int win = apply_move(&g_state->total, in.move);

            out.ok = 1;
            if (win)
            {
                snprintf(out.text, sizeof(out.text),
                         "You added %d. Total=%d. WIN! (Shared total resets).",
                         in.move, g_state->total);
                g_state->total = 0; // reset for now
            }
            else
            {
                snprintf(out.text, sizeof(out.text),
                         "You added %d. Shared Total=%d",
                         in.move, g_state->total);
            }

            sem_post(g_sem);

            write_all(rep_fd, &out, sizeof(out));
        }

        else if (in.type == MSG_QUIT)
        {
            out.ok = 1;
            snprintf(out.text, sizeof(out.text), "Goodbye %s.", player_name);
            write_all(rep_fd, &out, sizeof(out));
            break;
        }
        else
        {
            out.ok = 0;
            snprintf(out.text, sizeof(out.text), "Unknown message.");
            write_all(rep_fd, &out, sizeof(out));
        }
    }

    close(req_fd);
    close(rep_fd);
    unlink(req_fifo);
    _exit(0);
}

static int find_player_slot_by_pid(pid_t pid)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (g_state->players[i].active && g_state->players[i].pid == pid)
        {
            return i;
        }
    }
    return -1;
}

static int find_free_player_slot(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (!g_state->players[i].active)
            return i;
    }
    return -1;
}

static void debug_print_players(void)
{
    printf("[SERVER] Players (count=%d, turn=%d):\n", g_state->player_count, g_state->current_turn);
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (g_state->players[i].active)
        {
            printf("  slot %d: %s (pid=%d)\n", i, g_state->players[i].name, (int)g_state->players[i].pid);
        }
    }
}

int main(void)
{
    struct sigaction sa_int = {0}, sa_chld = {0};
    sa_int.sa_handler = on_sigint;
    sigaction(SIGINT, &sa_int, NULL);

    sa_chld.sa_handler = on_sigchld;
    sa_chld.sa_flags = 0;

#ifdef SA_RESTART
    sa_chld.sa_flags |= SA_RESTART;
#endif

#ifdef SA_NOCLDSTOP
    sa_chld.sa_flags |= SA_NOCLDSTOP;
#endif

    sigaction(SIGCHLD, &sa_chld, NULL);

    if (shared_init(&g_state, &g_sem, 1) != 0)
    {
        fprintf(stderr, "Failed to init shared state.\n");
        return 1;
    }

    if (safe_mkfifo(LOBBY_FIFO) != 0)
    {
        fprintf(stderr, "Failed to create lobby FIFO.\n");
        return 1;
    }

    int lobby_r = open(LOBBY_FIFO, O_RDONLY);
    if (lobby_r < 0)
    {
        perror("open lobby read");
        unlink(LOBBY_FIFO);
        return 1;
    }

    int lobby_w_dummy = open(LOBBY_FIFO, O_WRONLY);
    if (lobby_w_dummy < 0)
    {
        perror("open lobby dummy write");
        close(lobby_r);
        unlink(LOBBY_FIFO);
        return 1;
    }

    printf("[SERVER] Lobby ready: %s\n", LOBBY_FIFO);
    printf("[SERVER] Waiting for JOIN...\n");

    while (g_running)
    {
        ClientMsg join = {0};
        int rr = read_all(lobby_r, &join, sizeof(join));

        if (rr <= 0)
        {
            if (errno == EINTR)
                continue;
            usleep(50 * 1000);
            continue;
        }

        if (join.type != MSG_JOIN)
        {
            continue;
        }
        sem_wait(g_sem);

        int existing = find_player_slot_by_pid((pid_t)join.pid);
        if (existing == -1)
        {
            int slot = find_free_player_slot();
            if (slot == -1)
            {
                sem_post(g_sem);

                // Server full: reply to client then continue
                int rep_fd_full = open(join.reply_fifo, O_WRONLY);
                if (rep_fd_full >= 0)
                {
                    ServerMsg full = {0};
                    full.ok = 0;
                    snprintf(full.text, sizeof(full.text),
                             "Server full (max %d players). Try again later.", MAX_PLAYERS);
                    write_all(rep_fd_full, &full, sizeof(full));
                    close(rep_fd_full);
                }
                continue;
            }

            g_state->players[slot].active = 1;
            g_state->players[slot].pid = (pid_t)join.pid;
            snprintf(g_state->players[slot].name, sizeof(g_state->players[slot].name),
                     "%s", join.name);

            g_state->player_count++;

            // First player becomes the first turn
            if (g_state->player_count == 1)
            {
                g_state->current_turn = slot;
            }
        }

        // Optional debug print (shows list + current turn)
        debug_print_players();

        sem_post(g_sem);

        char req_fifo[MAX_PATH];
        snprintf(req_fifo, sizeof(req_fifo), "/tmp/race30_req_%d.fifo", (int)join.pid);

        if (safe_mkfifo(req_fifo) != 0)
        {
            continue;
        }

        int rep_fd = open(join.reply_fifo, O_WRONLY);
        if (rep_fd < 0)
        {
            perror("open client reply fifo");
            unlink(req_fifo);
            continue;
        }

       int snapshot;
sem_wait(g_sem);
snapshot = g_state->total;
sem_post(g_sem);

ServerMsg resp = {0};
resp.ok = 1;
snprintf(resp.text, sizeof(resp.text),
         "JOIN OK. Hi %s! Shared total=%d. Assigned request FIFO: %s",
         join.name, snapshot, req_fifo);
snprintf(resp.assigned_req_fifo, sizeof(resp.assigned_req_fifo), "%s", req_fifo);


        (void)write_all(rep_fd, &resp, sizeof(resp));
        close(rep_fd);

        pid_t cpid = fork();
        if (cpid < 0)
        {
            perror("fork");
            unlink(req_fifo);
            continue;
        }
        if (cpid == 0)
        {
            child_session_loop(req_fifo, join.reply_fifo, join.name);
        }

        printf("[SERVER] Player joined: %s (pid=%d) child=%d\n",
               join.name, (int)join.pid, (int)cpid);
    } // closes while (g_running)

    printf("\n[SERVER] Shutdown.\n");
    close(lobby_r);
    close(lobby_w_dummy);
    unlink(LOBBY_FIFO);
    shared_cleanup(g_state, g_sem, 1);

    return 0;
} // closes main
