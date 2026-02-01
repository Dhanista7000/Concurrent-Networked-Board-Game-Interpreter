#ifndef COMMON_H
#define COMMON_H

#include <pthread.h>
#include <stdbool.h>

// --- Constants ---
#define PORT 8080
#define MAX_PLAYERS 5           // Constraint: 3 to 5 players [cite: 24]
#define MIN_PLAYERS 3
#define DECK_SIZE 52
#define NAME_LEN 32
#define SHM_NAME "/game_shm"    // Name for POSIX Shared Memory

// --- Game Status Enum ---
typedef enum {
    WAITING_FOR_PLAYERS,
    GAME_START,
    PLAYER_TURN,
    ROUND_END,
    GAME_OVER
} GameStatus;

// --- Card Structure ---
typedef struct {
    char suit; // 'H', 'D', 'C', 'S'
    int value; // 2-14 (11=J, 12=Q, 13=K, 14=A)
} Card;

// --- Player Structure ---
// Lives in Shared Memory. Updated by Child Processes.
typedef struct {
    int pid;                    // Process ID of the handler
    int player_id;              // 0 to 4
    char name[NAME_LEN];
    int score;                  // Loaded from scores.txt 
    bool is_active;             // For skipping disconnected players [cite: 59]
    Card hand[2];               // Example: 2 cards per player
    bool has_played_this_round; // Flag for synchronization
} Player;

// --- SHARED MEMORY STATE ---
// This is the CRITICAL resource protected by Mutexes [cite: 62]
typedef struct {
    // Synchronization Primitives
    // MUST be initialized with PTHREAD_PROCESS_SHARED 
    pthread_mutex_t mutex;      
    pthread_cond_t turn_cond;   // To signal players when it's their move
    pthread_cond_t turn_complete_cond; // (IM DONE BELL)
    
    // Game Data
    Player players[MAX_PLAYERS];
    int player_count;
    int current_turn_index;     // Controlled by RR Scheduler Thread 
    GameStatus status;
    
    // Deck Management (Server-side randomness) 
    Card deck[DECK_SIZE];
    int deck_top_index;
    
    // --- NEW: LOGGER QUEUE (Rubric: Concurrent Logger) ---
    // [cite: 53] Protected by log_mutex
    char log_queue[50][256]; // Buffer for 50 messages
    int log_in;              // Where to write next
    int log_out;             // Where to read next
    int log_count;           // How many messages are waiting
    
    pthread_mutex_t log_mutex;
    pthread_cond_t log_cond;
    
} SharedState;

#endif // COMMON_H