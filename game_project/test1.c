#include <stdio.h>

#define MAX_TOTAL 30
#define MAX_MOVE 3

int main() {
    int total = 0;
    
    printf("=== FIRST TO 30 GAME ===\n");
    printf("Goal: Reach exactly %d!\n\n", MAX_TOTAL);
    
    while (total < MAX_TOTAL) {
        printf("Current total: %d/%d\n", total, MAX_TOTAL);
        printf("Enter your move (1-%d): ", MAX_MOVE);
        
        int move;
        scanf("%d", &move);
        
        if (move < 1 || move > MAX_MOVE) {
            printf("Invalid! Must be between 1 and %d.\n", MAX_MOVE);  
            continue;
        }
        
        if (total + move > MAX_TOTAL) {
            printf("Can't exceed %d! Try a smaller number.\n", MAX_TOTAL);
            continue;
        }
        
        total += move;
        
        if (total < MAX_TOTAL) {
            printf("New total: %d\n\n", total);
        }
    }
    
    printf("\nCONGRATULATIONS!\n");
    printf("You reached %d!\n", MAX_TOTAL);
    
    return 0;
}