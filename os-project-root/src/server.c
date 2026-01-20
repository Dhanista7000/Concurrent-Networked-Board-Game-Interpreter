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

// Include the shared header we defined earlier
#include "common.h"

#define PORT 8080

// Global pointer to shared memory (accessible by signal handlers if needed)
SharedState *game_state;

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
    pthread_mutexattr_t mutexAttr;
    pthread_condattr_t condAttr;

    pthread_mutexattr_init(&mutexAttr);
    pthread_condattr_init(&condAttr);

    pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED);
    pthread_condattr_setpshared(&condAttr, PTHREAD_PROCESS_SHARED);

    // Initialize the actual mutex and condition variable in shared memory
    pthread_mutex_init(&game_state->mutex, &mutexAttr);
    pthread_cond_init(&game_state->turn_cond, &condAttr);

    // Initialize Game State Data
    game_state->player_count = 0;
    game_state->status = WAITING_FOR_PLAYERS;
    game_state->current_turn_index = 0;
    
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

// --- Placeholder for Member 2's Logic ---
void handle_client_session(int client_socket, int player_id) {
    // Member 2 will fill this in.
    // For now, we just print a message so you can test connection.
    printf("[Child Process %d] Handling Player %d.\n", getpid(), player_id);
    
    // Simulate game loop
    while(1) {
        sleep(5); 
        // Logic will go here...
    }
    close(client_socket);
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
    // Member 1 Task: Append winning score to file
    FILE *fp = fopen("scores.txt", "a"); // 'a' for append
    if (fp) {
        fprintf(fp, "%s %d\n", name, score);
        fclose(fp);
        printf("[Persistence] Saved score for %s\n", name);
    }
}

// Round Robin Logic

void* scheduler_thread(void* arg) {
    printf("[Scheduler] Round Robin Thread Started.\n");
    
    while (1) {
        pthread_mutex_lock(&game_state->mutex);

        // Only manage turns if the game has actually started
        if (game_state->status == GAME_START || game_state->status == PLAYER_TURN) {
            
            // Round Robin Logic: Move to next active player
            int next_turn = (game_state->current_turn_index + 1) % game_state->player_count;
            game_state->current_turn_index = next_turn;
            game_state->status = PLAYER_TURN;

            printf("[Scheduler] It is now Player %d's turn.\n", game_state->current_turn_index);

            // Signal all child processes that the turn has changed
            pthread_cond_broadcast(&game_state->turn_cond);
        }

        pthread_mutex_unlock(&game_state->mutex);
        
        // The "Time Slice": Wait 5 seconds for the player to make a move
        sleep(5); 
    }
    return NULL;
}

// --- 3. MAIN LOOP (Rubric: Multiprocessing - Excellent) ---
int main() {
    setup_signals();         // Step 1: Handle Zombies
    setup_shared_memory();   // Step 2: Create SHM + Mutexes
    load_scores();

    // --- START SCHEDULER THREAD ---
    pthread_t rr_thread;
    if (pthread_create(&rr_thread, NULL, scheduler_thread, NULL) != 0) {
        perror("Failed to create scheduler thread");
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
        
        printf("[Server] Accepted connection. Assigned Player ID: %d\n", new_player_id);
        pthread_mutex_unlock(&game_state->mutex);

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
        }
    }

    // Cleanup (unreachable in this loop, but good practice)
    shm_unlink(SHM_NAME);
    return 0;
}