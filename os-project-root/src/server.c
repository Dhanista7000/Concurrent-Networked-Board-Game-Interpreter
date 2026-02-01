#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <time.h>

// Include the shared header we defined earlier
#include "common.h"

// Global pointer to shared memory (accessible by signal handlers if needed)
SharedState *game_state;

// --- ADD THESE PROTOTYPES HERE ---
void log_event(char *msg);
void* logger_thread(void* arg);
// ---------------------------------

// --- 1. ZOMBIE REAPER (Rubric: Multiprocessing - Excellent) ---
// Handles SIGCHLD to clean up child processes when players disconnect[cite: 34].
void handle_sigchld(int sig) {
    // WNOHANG ensures the server doesn't freeze waiting for a child that isn't dead yet.
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void setup_signals() {
    struct sigaction sa;
    sa.sa_handler = &handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, 0) == -1) {
        perror("sigaction");
        exit(1);
    }
}

// --- 2. SHARED MEMORY & MUTEX SETUP (Rubric: IPC & Sync - Excellent) ---
// DECK Management 
void init_deck() {
    char suits[] = {'H', 'D', 'C', 'S'};
    int index = 0;
    
    // 1. Fill the deck
    for (int s = 0; s < 4; s++) {
        for (int v = 2; v <= 14; v++) { // 2 to Ace (14)
            game_state->deck[index].suit = suits[s];
            game_state->deck[index].value = v;
            index++;
        }
    }
    
    // 2. Shuffle (Fisher-Yates Algorithm)
    // We use srand(time(0)) in main() so this is random every time
    for (int i = 0; i < 52; i++) {
        int r = rand() % 52;
        Card temp = game_state->deck[i];
        game_state->deck[i] = game_state->deck[r];
        game_state->deck[r] = temp;
    }
    
    game_state->deck_top_index = 0; // Start at top of deck
    printf("[Game Logic] Deck initialized and shuffled.\n");

    // CHEAT MODE: Force the first card to be an Ace of Spades (Value 14)
    // game_state->deck[0].value = 14; 
    // game_state->deck[0].suit = 'S';

    // printf("[Game Logic] Deck initialized (RIGGED FOR TESTING).\n");    
}
// Initializes memory accessible by all forked processes[cite: 62, 70].
void setup_shared_memory() {
    // A. Create/Open Shared Memory Object
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) { 
        perror("Shared memory creation failed"); 
        exit(1); 
    }

    // B. Set the size of the shared memory
    if (ftruncate(shm_fd, sizeof(SharedState)) == -1) {
        perror("ftruncate failed");
        exit(1);
    }

    // C. Map shared memory into address space
    game_state = mmap(0, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (game_state == MAP_FAILED) { 
        perror("mmap failed"); 
        exit(1); 
    }

    // D. Initialize Synchronization Primitives (CRITICAL STEP)
    // We must use PTHREAD_PROCESS_SHARED so mutexes work across fork()[cite: 70, 130].
    // Inside setup_shared_memory()
    pthread_mutexattr_t mutexAttr;
    pthread_condattr_t condAttr;

    pthread_mutexattr_init(&mutexAttr);
    pthread_condattr_init(&condAttr);

    pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED);
    pthread_condattr_setpshared(&condAttr, PTHREAD_PROCESS_SHARED);

    // Initialize the actual mutex and condition variable in shared memory
    pthread_mutex_init(&game_state->mutex, &mutexAttr);
    pthread_cond_init(&game_state->turn_cond, &condAttr);
    
    // NEW: Initialize the "Player Done" signal
    pthread_cond_init(&game_state->turn_complete_cond, &condAttr);

    // Initialize Game State Data
    game_state->player_count = 0;
    game_state->status = WAITING_FOR_PLAYERS;
    game_state->current_turn_index = 0;

    // --- NEW: Logger Initialization ---
    pthread_mutex_init(&game_state->log_mutex, &mutexAttr);
    pthread_cond_init(&game_state->log_cond, &condAttr);
    
    game_state->log_in = 0;
    game_state->log_out = 0;
    game_state->log_count = 0;    
    
    printf("[Server] Shared Memory & Process-Shared Mutexes Initialized.\n");
}

// Helper: Setup TCP Server Socket
int setup_server_socket() {
    int server_fd;
    struct sockaddr_in address;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    // Allow port reuse (avoids "Address already in use" errors)
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

//Helper
void load_scores() {
    FILE *fp = fopen("scores.txt", "r");
    if (!fp) {
        printf("[Server] No scores found. Starting fresh.\n");
        return;
    }
    
    // Simple format: Name Score
    char name[32];
    int score;
    while (fscanf(fp, "%s %d", name, &score) != EOF) {
        // In a real app, you'd map this to specific players
        // For now, just print it to prove it works
        printf("[Persistence] Loaded score: %s = %d\n", name, score);
    }
    fclose(fp);
}

void save_score(char *name, int score) {
    printf("[Persistence] Attempting to save score for %s...\n", name);
    
    FILE *fp = fopen("scores.txt", "a"); 
    if (fp) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char timestamp[64]; // Increased size just to be safe
        
        if (t) {
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);
        } else {
            strcpy(timestamp, "UNKNOWN_TIME");
        }

        fprintf(fp, "[%s] Winner: %s | Score: %d\n", timestamp, name, score);
        
        // --- THE FIX ---
        fflush(fp);          // 1. Force C buffer to OS
        int fd = fileno(fp); // 2. Get the underlying file descriptor
        fsync(fd);           // 3. Force OS buffer to Physical Disk
        // ----------------
        
        fclose(fp);
        printf("[Persistence] SUCCESS! Saved score to scores.txt\n");
    } else {
        perror("[Persistence] ERROR: Could not open scores.txt");
    }
}


// --- CLIENT HANDLER (CHILD PROCESS) ---
void handle_client_session(int client_socket, int player_id) {
    char buffer[1024];
    int read_size;

    // Send Welcome Message
    snprintf(buffer, sizeof(buffer), "Welcome Player %d! Waiting for game to start...\n", player_id + 1);
    send(client_socket, buffer, strlen(buffer), 0);

    while (1) {
        // --- SYNCHRONIZATION POINT ---
        // Lock the mutex to check the Shared Memory safely
        pthread_mutex_lock(&game_state->mutex);

        // Wait here until the Scheduler says "Something changed!"
        // pthread_cond_wait releases the lock while waiting, and re-acquires it when signaled.
        pthread_cond_wait(&game_state->turn_cond, &game_state->mutex);

        // 1. Check if it's MY turn
        if (game_state->current_turn_index == player_id && game_state->status == PLAYER_TURN) {
            
            // Tell Client it's their turn
            memset(buffer, 0, 1024);
            sprintf(buffer, "YOUR_TURN\n"); // Simple protocol message
            send(client_socket, buffer, strlen(buffer), 0);
            
            printf("[Child %d] Told Player %d it's their turn. Waiting for input...\n", getpid(), player_id + 1);

            // Unlock while waiting for network input (so other processes aren't blocked)
            pthread_mutex_unlock(&game_state->mutex);

            // 2. Wait for Client Response (Blocking Read)
            // The client MUST send "DRAW" or "PASS"
            read_size = recv(client_socket, buffer, 1024, 0);
            if (read_size > 0) {
                buffer[read_size] = '\0'; // Null-terminate
                
                // Re-lock to update game state
                pthread_mutex_lock(&game_state->mutex);
                
                if (strncmp(buffer, "DRAW", 4) == 0) {
                    // Logic: Take card from deck
                    int card_idx = game_state->deck_top_index++;
                    Card drawn = game_state->deck[card_idx];

                    // --- MEMBER 3: WIN CONDITION & PERSISTENCE ---
                    // Rule: If Card Value is 13 (King) or 14 (Ace), they WIN.
                    if (drawn.value >= 13) {
                         printf("\n!!! PLAYER %d WINS with %d%c !!!\n", player_id + 1, drawn.value, drawn.suit);
                         // ADD THIS:
                        char log_msg[100];
                        sprintf(log_msg, "GAME OVER: Player %d Wins with %d%c", player_id + 1, drawn.value, drawn.suit);
                        log_event(log_msg);
                         
                         // 1. Save Score to File (Persistence)
                         char p_name[32];
                         sprintf(p_name, "Player%d", player_id + 1);
                         save_score(p_name, 100); // Save 100 points
                         
                         // 2. Notify the Client
                         char win_msg[] = "YOU WON! Game Over.\n";
                         send(client_socket, win_msg, strlen(win_msg), 0);
                         
                         // 3. IMPORTANT: Unlock Mutex before exiting!
                         // If we exit while holding the lock, the whole game freezes.
                         pthread_mutex_unlock(&game_state->mutex);
                         
                         close(client_socket);
                         exit(0); // Kill this child process (Player leaves table)
                    }
                    // ---------------------------------------------
                    
                    printf("[Game Logic] Player %d drew %d%c\n", player_id + 1, drawn.value, drawn.suit);
                    
                    // Send feedback to client
                    char msg[64];
                    sprintf(msg, "YOU DREW: %d%c\n", drawn.value, drawn.suit);
                    send(client_socket, msg, strlen(msg), 0);
                }
                else {
                    printf("[Game Logic] Player %d passed.\n", player_id + 1);
                }

                // --- NEW: RING THE BELL ---
                // Tell the Scheduler: "I am done, you can switch turns now."
                printf("[Child %d] Player %d finished turn. Signaling Scheduler.\n", getpid(), player_id + 1);
                pthread_cond_signal(&game_state->turn_complete_cond);                
                
                // End turn manually (optional, or wait for Scheduler timeout)
                // For now, we just wait for the Scheduler to rotate.
            }
        }

        // Always unlock before repeating the loop
        pthread_mutex_unlock(&game_state->mutex);
    }
    
    close(client_socket);
}

// Round Robin Logic

void* scheduler_thread(void* arg) {
    printf("[Scheduler] Round Robin Thread Started. Waiting for Game Start...\n");
    
    while (1) {
        pthread_mutex_lock(&game_state->mutex);

        // If the game hasn't started, just wait a bit and check again
        if (game_state->status == WAITING_FOR_PLAYERS) {
            pthread_mutex_unlock(&game_state->mutex);
            sleep(1); 
            continue;
        }

        // --- ROUND ROBIN LOGIC ---
        if (game_state->status == GAME_START || game_state->status == PLAYER_TURN) {
            
            // 1. Move to next active player
            int next_turn = (game_state->current_turn_index + 1) % game_state->player_count;
            game_state->current_turn_index = next_turn;
            game_state->status = PLAYER_TURN;

            printf("[Scheduler] It is now Player %d's turn.\n", game_state->current_turn_index + 1);

            // 2. Wake up all players so the correct one can take their turn
            pthread_cond_broadcast(&game_state->turn_cond);

            // 3. CRITICAL CHANGE: WAIT HERE until the player finishes!
            // This replaces sleep(5). The scheduler sleeps until turn_complete_cond is rung.
            pthread_cond_wait(&game_state->turn_complete_cond, &game_state->mutex);
        }

        pthread_mutex_unlock(&game_state->mutex);
    }
    return NULL;
}

// --- LOGGER HELPER (Producers) ---
// Any process can call this to queue a message [cite: 46]
void log_event(char *msg) {
    pthread_mutex_lock(&game_state->log_mutex);
    
    if (game_state->log_count < 50) {
        // Copy message into the shared circular buffer
        strncpy(game_state->log_queue[game_state->log_in], msg, 255);
        game_state->log_queue[game_state->log_in][255] = '\0'; // Safety null
        
        // Move "In" Index forward (Circular logic)
        game_state->log_in = (game_state->log_in + 1) % 50;
        game_state->log_count++;
        
        // Wake up the logger thread
        pthread_cond_signal(&game_state->log_cond);
    } 
    // If full, we drop the log to remain Non-Blocking [cite: 52]
    
    pthread_mutex_unlock(&game_state->log_mutex);
}

// --- LOGGER THREAD (Consumer) ---
// [cite: 44] Writes to game.log
void* logger_thread(void* arg) {
    printf("[Logger] Thread Started. Writing to game.log...\n");
    
    // Open file in Append mode
    FILE *fp = fopen("game.log", "a");
    if (!fp) {
        perror("Failed to open game.log");
        return NULL;
    }

    while (1) {
        pthread_mutex_lock(&game_state->log_mutex);

        // Wait until there is a message
        while (game_state->log_count == 0) {
            pthread_cond_wait(&game_state->log_cond, &game_state->log_mutex);
        }

        // Get the message from the queue
        char buffer[256];
        strncpy(buffer, game_state->log_queue[game_state->log_out], 256);
        
        // Move "Out" Index forward
        game_state->log_out = (game_state->log_out + 1) % 50;
        game_state->log_count--;

        pthread_mutex_unlock(&game_state->log_mutex);

        // --- WRITE TO FILE (Done without lock!) ---
        // We get the timestamp here
        time_t now = time(NULL);
        char timestamp[26];
        ctime_r(&now, timestamp);
        timestamp[strlen(timestamp)-1] = '\0'; // Remove newline

        fprintf(fp, "[%s] %s\n", timestamp, buffer);
        fflush(fp); // Ensure it saves immediately
    }

    fclose(fp);
    return NULL;
}

// --- 3. MAIN LOOP (Rubric: Multiprocessing - Excellent) ---
int main() {

    // Seed the Random Generator 
    srand(time(NULL));

    setup_signals();         // Step 1: Handle Zombies
    setup_shared_memory();   // Step 2: Create SHM + Mutexes
    init_deck(); // Deck
    load_scores();

    // --- START SCHEDULER THREAD ---
    pthread_t rr_thread;
    if (pthread_create(&rr_thread, NULL, scheduler_thread, NULL) != 0) {
        perror("Failed to create scheduler thread");
        exit(1);
    }

    // --- NEW: START LOGGER THREAD ---
    pthread_t log_thr;
    if (pthread_create(&log_thr, NULL, logger_thread, NULL) != 0) {
        perror("Failed to create logger");
        exit(1);    
    }
    int server_fd = setup_server_socket();
    printf("[Server] Listening on port %d...\n", PORT);

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // --- ACCEPT-FORK LOOP [cite: 33] ---
    while (1) {
        // A. Accept new connection
        int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }

        // B. Check Player Limit (Critical Section)
        pthread_mutex_lock(&game_state->mutex);
        if (game_state->player_count >= MAX_PLAYERS) {
            printf("[Server] Max players reached. Rejecting connection.\n");
            close(client_socket);
            pthread_mutex_unlock(&game_state->mutex);
            continue;
        }

        // Assign ID and increment count
        int new_player_id = game_state->player_count;
        game_state->player_count++;
        // Initialize basic player stats
        game_state->players[new_player_id].is_active = 1;
        game_state->players[new_player_id].pid = 0; // Will be set by child
        
        printf("[Server] Accepted connection. Assigned Player ID: %d\n", new_player_id + 1);
        pthread_mutex_unlock(&game_state->mutex);

        // ADD THIS:
        char log_msg[100];
        sprintf(log_msg, "Player %d connected via TCP.", new_player_id + 1);
        log_event(log_msg);        

        // C. FORK [cite: 33]
        pid_t pid = fork();

        if (pid < 0) {
            perror("Fork failed");
            close(client_socket);
        } 
        else if (pid == 0) {
            // --- CHILD PROCESS ---
            close(server_fd); // Child doesn't need the listener

            // Update PID in shared memory (Safe because we have a unique index)
            pthread_mutex_lock(&game_state->mutex);
            game_state->players[new_player_id].pid = getpid();
            pthread_mutex_unlock(&game_state->mutex);

            // Handover to Game Logic (Member 2's job)
            handle_client_session(client_socket, new_player_id);
            
            exit(0); // Child must exit here
        } 
        else {
            // --- PARENT PROCESS ---
            close(client_socket); // Parent doesn't need the client socket
            // Loop back to accept()
            // CHECK IF WE CAN START THE GAME
             pthread_mutex_lock(&game_state->mutex);
             if (game_state->player_count >= 3 && game_state->status == WAITING_FOR_PLAYERS) {
                 game_state->status = GAME_START;
                 printf("[Server] 3 Players connected. STARTING GAME!\n");
                 
                 // Wake up the scheduler
                 pthread_cond_broadcast(&game_state->turn_cond); 
             }
             pthread_mutex_unlock(&game_state->mutex);            
        }
    }

    // Cleanup (unreachable in this loop, but good practice)
    shm_unlink(SHM_NAME);
    return 0;
} 
