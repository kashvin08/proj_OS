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
#include "include/state.h"

// Round-robin quantum (seconds)
#define RR_QUANTUM 30

static GameState *g_state = NULL;
static sem_t *g_sem = NULL;
static volatile sig_atomic_t g_running = 1;
static time_t g_turn_start_time = 0;

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

// Round-robin scheduler: advance to next active player
static void rr_advance_turn(void) {
    sem_wait(g_sem);
    
    if (g_state->player_count == 0) {
        sem_post(g_sem);
        return;
    }
    
    int start = g_state->current_turn;
    int next = start;
    int attempts = 0;
    
    do {
        next = (next + 1) % MAX_PLAYERS;
        attempts++;
        
        if (g_state->players[next].active) {
            g_state->current_turn = next;
            g_turn_start_time = time(NULL);
            break;
        }
        
        // If we've checked all slots and none are active, keep current
        if (attempts >= MAX_PLAYERS) {
            break;
        }
    } while (1);
    
    sem_post(g_sem);
}

// Check if current turn has timed out
static void rr_check_timeout(void) {
    sem_wait(g_sem);
    
    if (g_state->player_count == 0) {
        sem_post(g_sem);
        return;
    }
    
    time_t now = time(NULL);
    if (now - g_turn_start_time > RR_QUANTUM) {
        printf("[SERVER] Player %s timed out, skipping turn\n",
               g_state->players[g_state->current_turn].name);
        rr_advance_turn();
    }
    
    sem_post(g_sem);
}

// Find player slot by PID
static int find_player_slot_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_state->players[i].active && g_state->players[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

// Find first free player slot
static int find_free_player_slot(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_state->players[i].active) {
            return i;
        }
    }
    return -1;
}

// Debug function to show current state
static void debug_print_state(void) {
    printf("[SERVER] Total=%d, Players=%d, Current turn: ", 
           g_state->total, g_state->player_count);
    
    if (g_state->player_count > 0 && g_state->players[g_state->current_turn].active) {
        printf("%s (slot %d)\n", 
               g_state->players[g_state->current_turn].name, 
               g_state->current_turn);
    } else {
        printf("none\n");
    }
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_state->players[i].active) {
            printf("  Slot %d: %s (pid=%d)\n", 
                   i, g_state->players[i].name, (int)g_state->players[i].pid);
        }
    }
}

// Child process session handler
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
    
    // Send welcome message
    out.ok = 1;
    snprintf(out.text, sizeof(out.text), 
             "Welcome %s! You are connected. Type moves 1-3 or 'quit'.\n"
             "Game starts with 2+ players. Turns rotate automatically.",
             player_name);
    write_all(rep_fd, &out, sizeof(out));

    while (1) {
        ClientMsg in = {0};
        int rr = read_all(req_fd, &in, sizeof(in));
        if (rr <= 0) break;

        memset(&out, 0, sizeof(out));

        if (in.type == MSG_MOVE) {
            sem_wait(g_sem);
            
            // Find this player's slot
            int player_slot = find_player_slot_by_pid(client_pid);
            
            // Check if enough players
            if (g_state->player_count < 2) {
                sem_post(g_sem);
                out.ok = 0;
                snprintf(out.text, sizeof(out.text), 
                        "Need at least 2 players to start. Currently %d/2.",
                        g_state->player_count);
                write_all(rep_fd, &out, sizeof(out));
                continue;
            }
            
            // Check if it's this player's turn (RR enforcement)
            if (player_slot != g_state->current_turn) {
                int time_left = RR_QUANTUM - (int)(time(NULL) - g_turn_start_time);
                if (time_left < 0) time_left = 0;
                
                sem_post(g_sem);
                out.ok = 0;
                snprintf(out.text, sizeof(out.text), 
                        "Not your turn! Current turn: %s (%d seconds remaining)",
                        g_state->players[g_state->current_turn].name, time_left);
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
                out.ok = 1;
                snprintf(out.text, sizeof(out.text),
                        "WINNER! %s added %d. Total=%d! Game reset.",
                        player_name, in.move, g_state->total);
                
                // Reset game
                g_state->total = 0;
                rr_advance_turn();  // Start new round
            } else {
                // Advance to next player (RR)
                rr_advance_turn();
                out.ok = 1;
                snprintf(out.text, sizeof(out.text),
                        "Added %d. Total=%d. Next turn: %s",
                        in.move, g_state->total,
                        g_state->players[g_state->current_turn].name);
            }
            
            sem_post(g_sem);
            write_all(rep_fd, &out, sizeof(out));
            
        } else if (in.type == MSG_QUIT) {
            sem_wait(g_sem);
            
            // Remove player
            int slot = find_player_slot_by_pid(client_pid);
            if (slot >= 0) {
                g_state->players[slot].active = 0;
                g_state->player_count--;
                
                // If quitting player was current turn, advance
                if (slot == g_state->current_turn && g_state->player_count > 0) {
                    rr_advance_turn();
                }
            }
            
            sem_post(g_sem);
            
            out.ok = 1;
            snprintf(out.text, sizeof(out.text), "Goodbye %s.", player_name);
            write_all(rep_fd, &out, sizeof(out));
            break;
            
        } else {
            out.ok = 0;
            snprintf(out.text, sizeof(out.text), "Unknown message type.");
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
    printf("[SERVER] Round-robin with %d-second time quantum\n", RR_QUANTUM);
    printf("[SERVER] Waiting for JOIN requests...\n");

    fd_set readfds;
    struct timeval timeout;
    
    while (g_running) {
        // Check for RR timeouts
        rr_check_timeout();
        
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
        
        if (ret == 0) continue;
        
        if (FD_ISSET(lobby_r, &readfds)) {
            ClientMsg join = {0};
            int rr = read_all(lobby_r, &join, sizeof(join));
            
            if (rr <= 0) {
                usleep(10000);
                continue;
            }
            
            if (join.type != MSG_JOIN) continue;
            
            sem_wait(g_sem);
            
            // Check if player already exists
            int existing = find_player_slot_by_pid(join.pid);
            if (existing != -1) {
                sem_post(g_sem);
                
                // Send reconnection message
                int rep_fd = open(join.reply_fifo, O_WRONLY);
                if (rep_fd >= 0) {
                    ServerMsg resp = {0};
                    resp.ok = 1;
                    snprintf(resp.text, sizeof(resp.text),
                            "Reconnected as %s", join.name);
                    snprintf(resp.assigned_req_fifo, sizeof(resp.assigned_req_fifo),
                            "/tmp/race30_req_%d.fifo", (int)join.pid);
                    write_all(rep_fd, &resp, sizeof(resp));
                    close(rep_fd);
                }
                continue;
            }
            
            // Check for free slot
            int slot = find_free_player_slot();
            if (slot == -1) {
                sem_post(g_sem);
                
                int rep_fd = open(join.reply_fifo, O_WRONLY);
                if (rep_fd >= 0) {
                    ServerMsg resp = {0};
                    resp.ok = 0;
                    snprintf(resp.text, sizeof(resp.text),
                            "Server full (max %d players)", MAX_PLAYERS);
                    write_all(rep_fd, &resp, sizeof(resp));
                    close(rep_fd);
                }
                continue;
            }
            
            // Register new player
            g_state->players[slot].active = 1;
            g_state->players[slot].pid = join.pid;
            snprintf(g_state->players[slot].name, NAME_LEN, "%s", join.name);
            g_state->player_count++;
            
            // Set initial turn if first player
            if (g_state->player_count == 1) {
                g_state->current_turn = slot;
                g_turn_start_time = time(NULL);
            }
            
            debug_print_state();
            sem_post(g_sem);
            
            // Create player-specific request FIFO
            char req_fifo[MAX_PATH];
            snprintf(req_fifo, sizeof(req_fifo), "/tmp/race30_req_%d.fifo", (int)join.pid);
            
            if (safe_mkfifo(req_fifo) != 0) {
                continue;
            }
            
            // Send JOIN response
            int rep_fd = open(join.reply_fifo, O_WRONLY);
            if (rep_fd < 0) {
                perror("open reply fifo");
                unlink(req_fifo);
                continue;
            }
            
            ServerMsg resp = {0};
            resp.ok = 1;
            snprintf(resp.text, sizeof(resp.text),
                    "Joined as %s. Total players: %d/%d",
                    join.name, g_state->player_count, MAX_PLAYERS);
            snprintf(resp.assigned_req_fifo, sizeof(resp.assigned_req_fifo), "%s", req_fifo);
            
            write_all(rep_fd, &resp, sizeof(resp));
            close(rep_fd);
            
            // Fork child to handle this player
            pid_t child = fork();
            if (child < 0) {
                perror("fork");
                unlink(req_fifo);
                continue;
            }
            
            if (child == 0) {
                child_session_loop(req_fifo, join.reply_fifo, join.name, join.pid);
            }
            
            printf("[SERVER] New player: %s (pid=%d, slot=%d, child=%d)\n",
                   join.name, (int)join.pid, slot, (int)child);
        }
    }

    printf("\n[SERVER] Shutting down...\n");
    close(lobby_r);
    close(lobby_w_dummy);
    unlink(LOBBY_FIFO);
    shared_cleanup(g_state, g_sem, 1);
    return 0;
}
