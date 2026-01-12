#ifndef GAME_H
#define GAME_H

#include <stddef.h>

#define MAX_TOTAL 30
#define MAX_MOVE  3

// Returns 1 if valid, 0 if invalid. Writes a human-friendly error if invalid.
int validate_move(int current_total, int move, char *err, size_t errlen);

// Applies the move (assumes it is valid). Returns 1 if this move wins the game.
int apply_move(int *current_total, int move);

#endif
