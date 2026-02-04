#include <iostream>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <string>
#include <queue>
using namespace std;

// ---------------------------
// Shared memory layout
// (Your "4 integers" are in shared_int[])
// ---------------------------
struct SharedState {
    pthread_mutex_t shared_mutex;
    int shared_int[4];
};

// ---------------------------
// Logger queue (producer)
// ---------------------------
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  log_cv    = PTHREAD_COND_INITIALIZER;
static queue<string> log_queue;
static bool logger_running = true;

static const char* SHM_NAME = "/guess_game_shm_demo";
static const int MAX_PLAYERS = 4;

/* =========================================================
   =============== Member 4: Persistence ===================
   ========================================================= */

static int player_scores[MAX_PLAYERS] = {0};
static const char* SCORE_FILE = "scores.txt";

static string nowString();
static void logPush(const string& msg);
static void saveScores();
static volatile sig_atomic_t g_stop = 0;

// Load scores
static void loadScores() {
    FILE* fp = fopen(SCORE_FILE, "r");
    if (!fp) {
        logPush("[SCORE] No existing scores.txt, starting fresh.");
        return;
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        fscanf(fp, "%d", &player_scores[i]);
    }

    fclose(fp);
    logPush("[SCORE] Scores loaded from file.");
}

/* =========================================================
   =============== Member 3: Game Logic ====================
   ========================================================= */

static int secret_number = -1;
static int winner_id = -1;

// Generate a new secret number
static void generateSecretNumber() {
    srand(time(nullptr) ^ getpid());
    secret_number = (rand() % 100) + 1;  // 1 to 100
    logPush("[GAME] New secret number generated: " + to_string(secret_number));
}

// Process a guess from a player
static string processGuess(int player_id, int guess) {
    if (secret_number == -1) {
        generateSecretNumber();
    }

    if (guess == secret_number) {
        winner_id = player_id;
        player_scores[player_id]++;  // Increase score
        
        // Log win
        logPush("[GAME] Player " + to_string(player_id) + 
                " guessed " + to_string(guess) + " and WON!");
        
        return "WIN Correct! You guessed the number.";
    }
    else if (guess < secret_number) {
        return "HIGHER Guess higher";
    }
    else {
        return "LOWER Guess lower";
    }
}

// Start a new game
static void startNewGame() {
    generateSecretNumber();
    winner_id = -1;
    logPush("[GAME] New game started.");
}

/* =========================================================
   =============== Member 3: Client Handler ================
   ========================================================= */

static void handleClient(int player_id) {
    std::cout << "=== DEBUG handleClient: Player " << player_id << " starting ===" << std::endl;
    std::cerr << "Creating FIFO: /tmp/guess_game_client_" << player_id << std::endl;

    char fifo_name[100];
    snprintf(fifo_name, sizeof(fifo_name), 
             "/tmp/guess_game_client_%d", player_id);
    
    // Create FIFO for this client
    mkfifo(fifo_name, 0666);
    
    logPush("[CLIENT] Player " + to_string(player_id) + 
            " connected via " + string(fifo_name));
    
    int fd = open(fifo_name, O_RDONLY | O_NONBLOCK);
    
    while (true) {
        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        
        // Check if it's this player's turn
        int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd < 0) break;
        
        SharedState* st = (SharedState*)mmap(nullptr, sizeof(SharedState),
                                           PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        close(shm_fd);
        
        if (!st) break;
        
        pthread_mutex_lock(&st->shared_mutex);
        int current_player = st->shared_int[0];
        int game_over = st->shared_int[3];
        pthread_mutex_unlock(&st->shared_mutex);
        
        // ===== DEBUG ADDED HERE =====
        cout << "DEBUG Player " << player_id << ": My turn? current=" 
             << current_player << ", me=" << player_id << endl;
        // ===== END DEBUG =====
        
        munmap(st, sizeof(SharedState));
        
        if (game_over == 1) {
            break;  // Game ended
        }
        
        if (current_player != player_id) {
            // ===== DEBUG ADDED HERE =====
            cout << "DEBUG Player " << player_id << ": Not my turn, sleeping..." << endl;
            // ===== END DEBUG =====
            usleep(50000);  // 0.05s
            continue;
        }
        
        // Read client guess
        if (read(fd, buffer, sizeof(buffer)) > 0) {
            int guess;
            if (sscanf(buffer, "GUESS %*d %d", &guess) == 1) {

                logAppendDirect("[GAME] Player " + to_string(player_id) +
                    " guess number " + to_string(guess));

                string response = processGuess(player_id, guess);
                
                // Send response back to client
                int fd_resp = open(fifo_name, O_WRONLY);
                if (fd_resp > 0) {
                    write(fd_resp, response.c_str(), response.length() + 1);
                    close(fd_resp);
                }
                
                logPush("[CLIENT] Player " + to_string(player_id) + 
                        " guessed " + to_string(guess) + " -> " + response);
                        
                // If win, end game
                if (response.find("WIN") != string::npos) {
                    int shm_fd2 = shm_open(SHM_NAME, O_RDWR, 0666);
                    if (shm_fd2 < 0) break;
                    
                    SharedState* st2 = (SharedState*)mmap(nullptr, sizeof(SharedState),
                                                        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd2, 0);
                    close(shm_fd2);
                    
                    if (st2) {
                        pthread_mutex_lock(&st2->shared_mutex);
                        st2->shared_int[3] = 1;  // Game over
                        pthread_mutex_unlock(&st2->shared_mutex);
                        munmap(st2, sizeof(SharedState));
                    }
                    break;
                }
            }
        }
        
        usleep(100000);  // 0.1s delay
    }
    
    close(fd);
    unlink(fifo_name);
    logPush("[CLIENT] Player " + to_string(player_id) + " disconnected");
}

static void logAppendDirect(const string& msg) {
    int fd = open("game.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;

    flock(fd, LOCK_EX); // lock file (prevents mixed lines)

    string line = nowString() + " " + msg + "\n";
    write(fd, line.c_str(), line.size());

    flock(fd, LOCK_UN);
    close(fd);
}


// Save scores
static void saveScores() {
    FILE* fp = fopen(SCORE_FILE, "w");
    if (!fp) return;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        fprintf(fp, "%d\n", player_scores[i]);
    }

    fclose(fp);
    logPush("[SCORE] Scores saved to file.");
}

// SIGINT handler :only notify the program that it should terminate, no direct save data
// SIGINT handler: only notify program to stop
static void sigintHandler(int) {
    g_stop = 1;
}


// Reset game state but keep scores
static void resetGameState(SharedState* st) {
    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[0] = 0;
    st->shared_int[2] = -1;
    st->shared_int[3] = 0;
    pthread_mutex_unlock(&st->shared_mutex);

    logPush("[GAME] Game state reset. Scores preserved.");
}

/* =========================================================
   =================== Existing Code =======================
   ========================================================= */

// Helper: current time string
static string nowString() {
    char buf[64];
    time_t t = time(nullptr);
    tm tm{};
    localtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return string(buf);
}

// Push a log message (thread-safe)
static void logPush(const string& msg) {
    pthread_mutex_lock(&log_mutex);
    log_queue.push(nowString() + " " + msg);
    pthread_cond_signal(&log_cv);
    pthread_mutex_unlock(&log_mutex);
}

// ---------------------------
// Round Robin Scheduler Thread
// ---------------------------
struct SchedulerArgs {
    SharedState* st;
    int quantum_ms;
};

static int findNextConnected(int current, int connected_mask) {
    for (int step = 1; step <= MAX_PLAYERS; step++) {
        int next = (current + step) % MAX_PLAYERS;
        if (connected_mask & (1 << next)) return next;
    }
    return current;
}

static void* roundRobinThread(void* arg) {
    SchedulerArgs* a = (SchedulerArgs*)arg;
    SharedState* st = a->st;

    logPush("[SCHED] Round Robin scheduler started.");

    while (true) {
        usleep(a->quantum_ms * 1000);

        pthread_mutex_lock(&st->shared_mutex);

        int game_status    = st->shared_int[3];
        int current_player = st->shared_int[0];
        int connected_mask = st->shared_int[1];

        if (game_status != 0) {
            pthread_mutex_unlock(&st->shared_mutex);
            break;
        }

        if ((connected_mask & (1 << current_player)) == 0) {
            int fixed = findNextConnected(current_player, connected_mask);
            st->shared_int[0] = fixed;
            pthread_mutex_unlock(&st->shared_mutex);

            logPush("[SCHED] Current player disconnected -> skip to player " +
                    to_string(fixed));
            continue;
        }

        int next = findNextConnected(current_player, connected_mask);
        st->shared_int[0] = next;

        pthread_mutex_unlock(&st->shared_mutex);

        if (next != current_player) {
            logPush("[SCHED] It is now Player " + to_string(next) + "'s turn");
        }
    }

    logPush("[SCHED] Scheduler stopped.");
    return nullptr;
}

// ---------------------------
// Logger Thread
// ---------------------------
static void* loggerThread(void*) {
    FILE* fp = fopen("game.log", "a");
    if (!fp) return nullptr;

    logPush("[LOG] Logger started.");

    while (true) {
        pthread_mutex_lock(&log_mutex);
        while (log_queue.empty() && logger_running) {
            pthread_cond_wait(&log_cv, &log_mutex);
        }

        if (!logger_running && log_queue.empty()) {
            pthread_mutex_unlock(&log_mutex);
            break;
        }

        string msg = log_queue.front();
        log_queue.pop();
        pthread_mutex_unlock(&log_mutex);

        fprintf(fp, "%s\n", msg.c_str());
        fflush(fp);
    }

    fprintf(fp, "%s\n", (nowString() + " [LOG] Logger stopped.").c_str());
    fclose(fp);
    return nullptr;
}

// ---------------------------
// Process-shared mutex init
// ---------------------------
static void initProcessSharedMutex(pthread_mutex_t* mtx) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}

// ---------------------------
// Shared memory setup
// ---------------------------
static SharedState* createOrOpenSharedMemory(bool create_new) {
    int fd;
    if (create_new) {
        shm_unlink(SHM_NAME);
        fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    } else {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
    }

    if (fd < 0) return nullptr;

    if (create_new) {
        ftruncate(fd, sizeof(SharedState));
    }

    void* mem = mmap(nullptr, sizeof(SharedState),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    return (SharedState*)mem;
}

int main() {
    signal(SIGINT, sigintHandler);

    SharedState* st = createOrOpenSharedMemory(true);
    if (!st) return 1;

    memset(st, 0, sizeof(SharedState));
    initProcessSharedMutex(&st->shared_mutex);

    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[0] = 0;   // current player
    st->shared_int[1] = 0;   // connected_mask (start empty)
    st->shared_int[2] = -1;
    st->shared_int[3] = 0;   // game running
    pthread_mutex_unlock(&st->shared_mutex);

    loadScores();

    // ============ ADD THIS LINE ============
    startNewGame();  // Start the guessing game!
    // ============ END ADDED CODE ============

    printf("Server listening on port 8080...\n");
    printf("Waiting for players to connect...\n");

    // ---- Simulate players connecting ----
    for (int i = 0; i < MAX_PLAYERS; i++) {
        printf("Waiting for player %d to connect...\n", i + 1);
        sleep(1); // simulate delay

        pthread_mutex_lock(&st->shared_mutex);
        st->shared_int[1] |= (1 << i); // mark player connected
        pthread_mutex_unlock(&st->shared_mutex);

        printf("Player %d connected!\n", i + 1);
    }

    printf("Game started!\n");

    logPush("[MAIN] Forking client processes...");
    
    // Fork child processes for 4 players
    for (int i = 0; i < 4; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process: handle one client
            handleClient(i);
            exit(0);  // IMPORTANT: Exit after handling
        }
        else if (pid > 0) {
            printf("Forked player %d (PID: %d)\n", i, pid);
            logPush("[MAIN] Forked player " + to_string(i) + 
                    " (PID: " + to_string(pid) + ")");
        }
    }
        // ---- Scheduler thread ----
    SchedulerArgs schedArgs{st, 800};
    pthread_t sched_tid;
    pthread_create(&sched_tid, nullptr, roundRobinThread, &schedArgs);

    // ---- Logger thread ----
    pthread_t log_tid;
    pthread_create(&log_tid, nullptr, loggerThread, nullptr);
    
    logPush("[MAIN] Server running.");

    // ---- Main loop ----
    while (!g_stop) {
        usleep(100 * 1000);
    }

    printf("Server shutting down...\n");
    logPush("[SIGNAL] SIGINT received. Saving scores...");

    saveScores();

    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[3] = 1; // stop scheduler
    pthread_mutex_unlock(&st->shared_mutex);

    pthread_join(sched_tid, nullptr);

    pthread_mutex_lock(&log_mutex);
    logger_running = false;
    pthread_cond_signal(&log_cv);
    pthread_mutex_unlock(&log_mutex);

    pthread_join(log_tid, nullptr);

    munmap(st, sizeof(SharedState));
    shm_unlink(SHM_NAME);

    return 0;
}