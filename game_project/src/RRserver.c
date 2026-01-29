#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#include "include/protocol.h"
#include "include/game.h"
#include <semaphore.h>
#include "include/state.h"

static GameState *g_state = NULL;
static sem_t *g_sem = NULL;
static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) { (void)sig; g_running = 0; }
static void on_sigchld(int sig) { (void)sig; while (waitpid(-1, NULL, WNOHANG) > 0); }

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
            if (errno == EINTR) continue;
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
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 1;
}

// Round-robin scheduler functions
static void advance_turn(void) {
    sem_wait(g_sem);
    
    if (g_state->player_count < 2) {
        sem_post(g_sem);
        return;
    }
    
    int start = g_state->current_turn;
    int next = start;
    
    // Find next active player
    do {
        next = (next + 1) % MAX_PLAYERS;
        if (g_state->players[next].active) {
            g_state->current_turn = next;
            g_state->turn_start_time = time(NULL);
            break;
        }
    } while (next != start);
    
    sem_post(g_sem);
}

static void check_turn_timeout(void) {
    sem_wait(g_sem);
    
    if (g_state->player_count < 2 || !g_state->game_started) {
        sem_post(g_sem);
        return;
    }
    
    time_t now = time(NULL);
    if (now - g_state->turn_start_time > TIME_QUANTUM) {
        printf("[SERVER] Player %s timeout, skipping turn\n",
               g_state->players[g_state->current_turn].name);
        advance_turn();
        g_state->turn_start_time = now;
    }
    
    sem_post(g_sem);
}

static int find_player_slot_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_state->players[i].active && g_state->players[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static int find_free_player_slot(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_state->players[i].active) return i;
    }
    return -1;
}

static void debug_print_players(void) {
    printf("[SERVER] Players (count=%d, turn=%d):\n", g_state->player_count, g_state->current_turn);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_state->players[i].active) {
            printf("  slot %d: %s (pid=%d)\n", i, g_state->players[i].name, (int)g_state->players[i].pid);
        }
    }
}

static void notify_all_players(const char *message) {
    // This would notify all connected players about game events
    // For simplicity, we'll just print to console
    printf("[SERVER BROADCAST] %s\n", message);
}

static void child_session_loop(const char *req_fifo, const char *reply_fifo, const char *player_name, pid_t client_pid) {
    int req_fd = open(req_fifo, O_RDONLY);
    if (req_fd < 0) {
        perror("[CHILD] open req fifo");
        _exit(1);
    }

    int rep_fd = open(reply_fifo, O_WRONLY);
    if (rep_fd < 0) {
        perror("[CHILD] open reply fifo");
        close(req_fd);
        _exit(1);
    }

    ServerMsg out = {0};
    int player_slot = find_player_slot_by_pid(client_pid);
    
    // Send welcome message
    if (player_slot >= 0) {
        sem_wait(g_sem);
        out.ok = 1;
        if (g_state->player_count >= 2 && !g_state->game_started) {
            g_state->game_started = 1;
            snprintf(out.text, sizeof(out.text),
                    "Game started! Shared total=%d. Players take turns in round-robin order.",
                    g_state->total);
        } else if (g_state->game_started) {
            snprintf(out.text, sizeof(out.text),
                    "Welcome %s! Game already in progress. Shared total=%d. Current turn: %s",
                    player_name, g_state->total, g_state->players[g_state->current_turn].name);
        } else {
            snprintf(out.text, sizeof(out.text),
                    "Welcome %s! Waiting for more players (need %d total).",
                    player_name, 2 - g_state->player_count);
        }
        sem_post(g_sem);
        write_all(rep_fd, &out, sizeof(out));
    }

    while (1) {
        ClientMsg in = {0};
        int rr = read_all(req_fd, &in, sizeof(in));
        if (rr <= 0) break;

        memset(&out, 0, sizeof(out));

        if (in.type == MSG_MOVE) {
            sem_wait(g_sem);
            
            // Check if game has enough players
            if (g_state->player_count < 2) {
                sem_post(g_sem);
                out.ok = 0;
                snprintf(out.text, sizeof(out.text), 
                        "Waiting for more players (need %d total).", 2 - g_state->player_count);
                write_all(rep_fd, &out, sizeof(out));
                continue;
            }
            
            // Check if it's this player's turn
            int current_slot = find_player_slot_by_pid(in.pid);
            if (current_slot != g_state->current_turn) {
                sem_post(g_sem);
                out.ok = 0;
                snprintf(out.text, sizeof(out.text), 
                        "Not your turn! Waiting for %s's move (%d seconds remaining).",
                        g_state->players[g_state->current_turn].name,
                        TIME_QUANTUM - (int)(time(NULL) - g_state->turn_start_time));
                write_all(rep_fd, &out, sizeof(out));
                continue;
            }
            
            // Validate move
            char err[128];
            if (!validate_move(g_state->total, in.move, err, sizeof(err))) {
                sem_post(g_sem);
                out.ok = 0;
                snprintf(out.text, sizeof(out.text), "%s", err);
                write_all(rep_fd, &out, sizeof(out));
                continue;
            }
            
            // Apply move
            int win = apply_move(&g_state->total, in.move);
            
            if (win) {
                snprintf(out.text, sizeof(out.text),
                        "WINNER! %s added %d. Total=%d. Game over!",
                        player_name, in.move, g_state->total);
                
                // Reset game
                g_state->total = 0;
                g_state->game_started = 0;
                notify_all_players(out.text);
            } else {
                // Advance turn (round-robin)
                advance_turn();
                snprintf(out.text, sizeof(out.text),
                        "You added %d. Total=%d. Next turn: %s",
                        in.move, g_state->total, g_state->players[g_state->current_turn].name);
            }
            
            sem_post(g_sem);
            out.ok = 1;
            write_all(rep_fd, &out, sizeof(out));
            
        } else if (in.type == MSG_QUIT) {
            sem_wait(g_sem);
            
            int slot = find_player_slot_by_pid(in.pid);
            if (slot >= 0) {
                g_state->players[slot].active = 0;
                g_state->player_count--;
                
                // If quitting player was current turn, advance turn
                if (slot == g_state->current_turn && g_state->player_count >= 2) {
                    advance_turn();
                }
                
                // If not enough players, pause game
                if (g_state->player_count < 2) {
                    g_state->game_started = 0;
                    notify_all_players("Game paused - waiting for more players");
                }
            }
            
            sem_post(g_sem);
            
            out.ok = 1;
            snprintf(out.text, sizeof(out.text), "Goodbye %s.", player_name);
            write_all(rep_fd, &out, sizeof(out));
            break;
        } else {
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

int main(void) {
    struct sigaction sa_int = {0}, sa_chld = {0};
    sa_int.sa_handler = on_sigint;
    sigaction(SIGINT, &sa_int, NULL);

    sa_chld.sa_handler = on_sigchld;
    sa_chld.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    if (shared_init(&g_state, &g_sem, 1) != 0) {
        fprintf(stderr, "Failed to init shared state.\n");
        return 1;
    }

    if (safe_mkfifo(LOBBY_FIFO) != 0) {
        fprintf(stderr, "Failed to create lobby FIFO.\n");
        shared_cleanup(g_state, g_sem, 1);
        return 1;
    }

    int lobby_r = open(LOBBY_FIFO, O_RDONLY | O_NONBLOCK);
    if (lobby_r < 0) {
        perror("open lobby read");
        unlink(LOBBY_FIFO);
        shared_cleanup(g_state, g_sem, 1);
        return 1;
    }

    int lobby_w_dummy = open(LOBBY_FIFO, O_WRONLY);
    if (lobby_w_dummy < 0) {
        perror("open lobby dummy write");
        close(lobby_r);
        unlink(LOBBY_FIFO);
        shared_cleanup(g_state, g_sem, 1);
        return 1;
    }

    printf("[SERVER] Lobby ready: %s\n", LOBBY_FIFO);
    printf("[SERVER] Waiting for JOIN (Max %d players, %d seconds per turn)...\n", MAX_PLAYERS, TIME_QUANTUM);

    fd_set readfds;
    struct timeval timeout;
    
    while (g_running) {
        // Check for turn timeouts
        check_turn_timeout();
        
        FD_ZERO(&readfds);
        FD_SET(lobby_r, &readfds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int ret = select(lobby_r + 1, &readfds, NULL, NULL, &timeout);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (ret == 0) continue; // Timeout
        
        if (FD_ISSET(lobby_r, &readfds)) {
            ClientMsg join = {0};
            int rr = read_all(lobby_r, &join, sizeof(join));
            
            if (rr <= 0) {
                if (errno == EINTR) continue;
                usleep(10000);
                continue;
            }
            
            if (join.type != MSG_JOIN) continue;
            
            sem_wait(g_sem);
            
            int existing = find_player_slot_by_pid((pid_t)join.pid);
            if (existing != -1) {
                // Player reconnecting
                sem_post(g_sem);
                
                char req_fifo[MAX_PATH];
                snprintf(req_fifo, sizeof(req_fifo), "/tmp/race30_req_%d.fifo", (int)join.pid);
                
                int rep_fd = open(join.reply_fifo, O_WRONLY);
                if (rep_fd >= 0) {
                    ServerMsg resp = {0};
                    resp.ok = 1;
                    snprintf(resp.text, sizeof(resp.text),
                            "Welcome back %s! Reconnected to existing session.",
                            join.name);
                    snprintf(resp.assigned_req_fifo, sizeof(resp.assigned_req_fifo), "%s", req_fifo);
                    write_all(rep_fd, &resp, sizeof(resp));
                    close(rep_fd);
                }
                continue;
            }
            
            int slot = find_free_player_slot();
            if (slot == -1) {
                sem_post(g_sem);
                
                int rep_fd_full = open(join.reply_fifo, O_WRONLY);
                if (rep_fd_full >= 0) {
                    ServerMsg full = {0};
                    full.ok = 0;
                    snprintf(full.text, sizeof(full.text),
                            "Server full (max %d players). Try again later.", MAX_PLAYERS);
                    write_all(rep_fd_full, &full, sizeof(full));
                    close(rep_fd_full);
                }
                continue;
            }
            
            // Register new player
            g_state->players[slot].active = 1;
            g_state->players[slot].pid = (pid_t)join.pid;
            snprintf(g_state->players[slot].name, sizeof(g_state->players[slot].name),
                    "%s", join.name);
            g_state->player_count++;
            
            // If first player or game not started, set turn to this player
            if (g_state->player_count == 1 || !g_state->game_started) {
                g_state->current_turn = slot;
                g_state->turn_start_time = time(NULL);
            }
            
            debug_print_players();
            sem_post(g_sem);
            
            char req_fifo[MAX_PATH];
            snprintf(req_fifo, sizeof(req_fifo), "/tmp/race30_req_%d.fifo", (int)join.pid);
            
            if (safe_mkfifo(req_fifo) != 0) {
                continue;
            }
            
            int rep_fd = open(join.reply_fifo, O_WRONLY);
            if (rep_fd < 0) {
                perror("open client reply fifo");
                unlink(req_fifo);
                continue;
            }
            
            ServerMsg resp = {0};
            resp.ok = 1;
            snprintf(resp.text, sizeof(resp.text),
                    "JOIN OK. Hi %s! Assigned request FIFO: %s",
                    join.name, req_fifo);
            snprintf(resp.assigned_req_fifo, sizeof(resp.assigned_req_fifo), "%s", req_fifo);
            
            write_all(rep_fd, &resp, sizeof(resp));
            close(rep_fd);
            
            pid_t cpid = fork();
            if (cpid < 0) {
                perror("fork");
                unlink(req_fifo);
                continue;
            }
            
            if (cpid == 0) {
                child_session_loop(req_fifo, join.reply_fifo, join.name, join.pid);
            }
            
            printf("[SERVER] Player joined: %s (pid=%d) slot=%d child=%d\n",
                   join.name, (int)join.pid, slot, (int)cpid);
        }
    }

    printf("\n[SERVER] Shutdown.\n");
    close(lobby_r);
    close(lobby_w_dummy);
    unlink(LOBBY_FIFO);
    shared_cleanup(g_state, g_sem, 1);
    return 0;
}
