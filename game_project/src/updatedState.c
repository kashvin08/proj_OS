#include "include/state.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

int shared_init(GameState **out_state, sem_t **out_sem, int reset) {
    if (reset) {
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_NAME);
    }

    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return -1; }

    if (ftruncate(fd, sizeof(GameState)) != 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    void *addr = mmap(NULL, sizeof(GameState),
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) { perror("mmap"); return -1; }

    sem_t *sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) { perror("sem_open"); munmap(addr, sizeof(GameState)); return -1; }

    GameState *st = (GameState*)addr;
    if (reset) {
        memset(st, 0, sizeof(*st));
        st->total = 0;
        st->turn_start_time = time(NULL);
        st->game_started = 0;
    }

    *out_state = st;
    *out_sem = sem;
    return 0;
}

void shared_cleanup(GameState *state, sem_t *sem, int unlink_ipc) {
    if (state) munmap(state, sizeof(GameState));
    if (sem && sem != SEM_FAILED) sem_close(sem);
    if (unlink_ipc) {
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_NAME);
    }
}
