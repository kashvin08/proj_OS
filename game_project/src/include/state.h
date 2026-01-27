#ifndef STATE_H
#define STATE_H

#include <semaphore.h>
#include <sys/types.h>  // pid_t

#define SHM_NAME "/race30_shm"
#define SEM_NAME "/race30_sem"

#define MAX_PLAYERS 5
#define NAME_LEN 32

typedef struct {
    pid_t pid;
    char  name[NAME_LEN];
    int   active;   // 1=active, 0=free slot
} Player;

typedef struct {
    int total;

    Player players[MAX_PLAYERS];
    int player_count;

    int current_turn;   // index 0..player_count-1 (only valid among active players)
} GameState;

int shared_init(GameState **out_state, sem_t **out_sem, int reset);
void shared_cleanup(GameState *state, sem_t *sem, int unlink_ipc);

#endif
