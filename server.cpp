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
            logPush("[SCHED] Turn moved: " + to_string(current_player) +
                    " -> " + to_string(next));
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
    st->shared_int[0] = 0;
    st->shared_int[1] = 0b1111;
    st->shared_int[2] = -1;
    st->shared_int[3] = 0;
    pthread_mutex_unlock(&st->shared_mutex);

    loadScores();

    pthread_t log_tid;
    pthread_create(&log_tid, nullptr, loggerThread, nullptr);

    SchedulerArgs schedArgs{st, 800};
    pthread_t sched_tid;
    pthread_create(&sched_tid, nullptr, roundRobinThread, &schedArgs);

    logPush("[MAIN] Demo started. Watch game.log.");

    while (!g_stop) {
        usleep(100 * 1000); // 0.1s
        }
        
    logPush("[SIGNAL] SIGINT received. Saving scores...");
    saveScores();
        
    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[3] = 1;
    pthread_mutex_unlock(&st->shared_mutex);

    pthread_join(sched_tid, nullptr);

    saveScores();

    pthread_mutex_lock(&log_mutex);
    logger_running = false;
    pthread_cond_signal(&log_cv);
    pthread_mutex_unlock(&log_mutex);

    pthread_join(log_tid, nullptr);

    munmap(st, sizeof(SharedState));
    shm_unlink(SHM_NAME);

    return 0;
}