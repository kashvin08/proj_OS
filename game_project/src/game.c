#include "include/game.h"
#include <stdio.h>

int validate_move(int current_total, int move, char *err, size_t errlen) {
    if (move < 1 || move > MAX_MOVE) {
        if (err && errlen) {
            snprintf(err, errlen, "Invalid move. Must be between 1 and %d.", MAX_MOVE);
        }
        return 0;
    }
    if (current_total + move > MAX_TOTAL) {
        if (err && errlen) {
            snprintf(err, errlen, "Invalid move. Total cannot exceed %d.", MAX_TOTAL);
        }
        return 0;
    }
    return 1;
}

int apply_move(int *current_total, int move) {
    *current_total += move;
    return (*current_total == MAX_TOTAL);
}
