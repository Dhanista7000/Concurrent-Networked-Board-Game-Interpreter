// Member 3: Scores loading/saving logic
#include "common.h"
#include <stdio.h>

void load_all_scores(SharedState *state) {
    FILE *fp = fopen("data/scores.txt", "r");
    if (!fp) return;

    char name[NAME_LEN];
    int win_count;
    int i = 0;
    while (fscanf(fp, "%s %d", name, &win_count) != EOF && i < MAX_PLAYERS) {
        // You can logic this to match names, or just load them into the players
        printf("[Persistence] Loaded: %s with %d wins\n", name, win_count);
        i++;
    }
    fclose(fp);
}