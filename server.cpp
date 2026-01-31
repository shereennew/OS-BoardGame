#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <string>
#include <queue>
using namespace std;

// mee hui

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>

#define SERVER_PORT 5555
#define BACKLOG 10

static int listen_fd;
static SharedState* g_state = nullptr;


// ---------------------------
// Shared memory layout
// (Your "4 integers" are in shared_int[])
// ---------------------------
struct SharedState {
    // Process-shared mutex to protect the 4 integers below
    pthread_mutex_t shared_mutex;
    int shared_int[4];
};


// SIGCHLD handler (zombie cleanup)
static void sigchld_handler(int signo) {
    (void)signo;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// Setup SIGCHLD
static void setup_sigchld() {
    struct sigaction sa{};
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

// TCP socket setup
static int setup_server_socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }

    return fd;
}

/* Child process: client session
   (Game logic handled elsewhere) */
static void client_process(int client_fd, int player_id) {
    close(listen_fd); // child does NOT accept()

    char buf[128];
    snprintf(buf, sizeof(buf), "WELCOME PLAYER %d\n", player_id);
    write(client_fd, buf, strlen(buf));

    // ---- Example idle loop ----

    while (1) {
        sleep(1);

        pthread_mutex_lock(&g_state->shared_mutex);
        if (g_state->shared_int[3] != 0) {
            pthread_mutex_unlock(&g_state->shared_mutex);
            break;
        }
        pthread_mutex_unlock(&g_state->shared_mutex);
    }

    /* mark disconnected */
    pthread_mutex_lock(&g_state->shared_mutex);
    g_state->shared_int[1] &= ~(1 << player_id);
    pthread_mutex_unlock(&g_state->shared_mutex);

    close(client_fd);
    exit(0);
}


// ---------------------------
// Logger queue (producer)
// ---------------------------
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER; //prevent race condition
static pthread_cond_t  log_cv    = PTHREAD_COND_INITIALIZER;  //condition variable: avoid busy waiting
static queue<string> log_queue;                               //FIFO queue that stores log messages temporarily
static bool logger_running = true;                            //A control flag for the logger thread

static const char* SHM_NAME = "/guess_game_shm_demo";         //Name of the POSIX shared memory object
static const int MAX_PLAYERS = 4;

// Helper: current time string
static string nowString() {
    char buf[64];                                               //Temporary buffer to store formatted time text
    time_t t = time(nullptr);                                   //Gets the current system time
    tm tm{};                                                    //store broken-down time
    localtime_r(&t, &tm);                                       //Converts time_t into local time (thread-safe)
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", //snprinf: writes the string pointed to by format to buffer
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return string(buf);
}

// Push a log message (thread-safe)
static void logPush(const string& msg) {        
    pthread_mutex_lock(&log_mutex);                //Locks the mutex
    log_queue.push(nowString() + " " + msg);       //Pushes the nowString() into the queue
    pthread_cond_signal(&log_cv);                  //Wakes up the logger thread
    pthread_mutex_unlock(&log_mutex);              //unlock the mutex
}

// ---------------------------
// Round Robin Scheduler Thread
// Updates shared_int[0] = current_player
// Skips disconnected players using shared_int[1] = connected_mask
// ---------------------------
struct SchedulerArgs {
    SharedState* st;        ////pointer to your shared memory struct
    int quantum_ms;
};

static int findNextConnected(int current, int connected_mask) {   //skip disconnected players
    // Try the next players in RR order. If none connected, return current.
    for (int step = 1; step <= MAX_PLAYERS; step++) {
        int next = (current + step) % MAX_PLAYERS;
        if (connected_mask & (1 << next)) return next;
        /*integer used like bits
        connected_mask = 0b1111 (15) (all connected)
        connected_mask = 0b1101 (13) (player 1 disconnected)
        */
    }
    return current; // nobody else connected
}

static void* roundRobinThread(void* arg) {
    SchedulerArgs* a = (SchedulerArgs*)arg;    //pointer to your shared memory struct
    SharedState* st = a->st;

    logPush("[SCHED] Round Robin scheduler started.");

    while (true) {
        // sleep for the time quantum
        usleep(a->quantum_ms * 1000);          //Scheduler waits one time quantum microsecond

        // Lock shared memory before reading/writing the 4 ints
        pthread_mutex_lock(&st->shared_mutex);

        int game_status    = st->shared_int[3];
        int current_player = st->shared_int[0];
        int connected_mask = st->shared_int[1];

        if (game_status != 0) {
            pthread_mutex_unlock(&st->shared_mutex);
            break; // game ended
        }

        // If current player disconnected, skip immediately
        if ((connected_mask & (1 << current_player)) == 0) {
            int fixed = findNextConnected(current_player, connected_mask);
            st->shared_int[0] = fixed;
            pthread_mutex_unlock(&st->shared_mutex);

            logPush("[SCHED] Current player disconnected -> skip to player " + std::to_string(fixed));
            continue;
        }

        // Normal Round Robin turn change
        int next = findNextConnected(current_player, connected_mask);
        st->shared_int[0] = next;

        pthread_mutex_unlock(&st->shared_mutex);

        //Log the turn change
        if (next != current_player) {
            logPush("[SCHED] Turn moved: " + to_string(current_player) +
                    " -> " + to_string(next));
        }
    }

    logPush("[SCHED] Scheduler stopped.");
    return nullptr;
}

// ---------------------------
// Logger Thread (consumer)
// Writes queued messages to game.log
// ---------------------------
static void* loggerThread(void* /*unused*/) {           //pthread thread function
    FILE* fp = fopen("game.log", "a");             //Opens game.log
    if (!fp) return nullptr;

    logPush("[LOG] Logger started.");

    while (true) {
        pthread_mutex_lock(&log_mutex);
        while (log_queue.empty() && logger_running) {
            pthread_cond_wait(&log_cv, &log_mutex);     //wait for signal when there is nothing to log
        }

        if (!logger_running && log_queue.empty()) {
            pthread_mutex_unlock(&log_mutex);
            break;
        }

        string msg = log_queue.front();   //Get one log message from queue
        log_queue.pop();
        pthread_mutex_unlock(&log_mutex);

        fprintf(fp, "%s\n", msg.c_str());
        fflush(fp);
    }

    fprintf(fp, "%s\n", (nowString() + " [LOG] Logger stopped.").c_str());
    fflush(fp);
    fclose(fp);
    return nullptr;
}

// ---------------------------
// Create + init process-shared mutex in shared memory
// ---------------------------
static void initProcessSharedMutex(pthread_mutex_t* mtx) {    //Takes a pointer to a mutex that lives inside shared memory
    pthread_mutexattr_t attr;                                 //Creates a mutex attribute object to control how the mutex behaves
    pthread_mutexattr_init(&attr);


    // This is the key requirement: Make the mutex process-shared
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    //***Allows the mutex to be placed in shared memory used by multiple processes


    pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}

// ---------------------------
// Shared memory setup (shm_open + ftruncate + mmap)
// ---------------------------
static SharedState* createOrOpenSharedMemory(bool create_new) {
    int fd;                   //file descriptor 
    if (create_new) {
        shm_unlink(SHM_NAME); // remove old if exists 
        fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    } else {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
    }

    if (fd < 0) {
        perror("shm_open");
        return nullptr;
    }

    if (create_new) {
        if (ftruncate(fd, sizeof(SharedState)) != 0) {     //set the size of an open file to a specified length
            perror("ftruncate");
            close(fd);
            return nullptr;
        }
    }

    void* mem = mmap(nullptr, sizeof(SharedState),         //attaches the shared memory into your process and returns a pointer.
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (mem == MAP_FAILED) {
        perror("mmap");
        return nullptr;
    }

    return (SharedState*)mem;
}

int main() {

    setup_sigchld();

    // 1) Create shared memory (server side)
    SharedState* st = createOrOpenSharedMemory(true);
    if (!st) return 1;

    g_state = st;   

    // 2) Initialize mutex + shared ints (ONLY once, server side)
    std::memset(st, 0, sizeof(SharedState));
    initProcessSharedMutex(&st->shared_mutex);

    // Protect shared memory while setting initial values
    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[0] = 0;                  // current_player
    st->shared_int[1] = 0b1111;             // connected_mask (players 0-3 connected)
    st->shared_int[2] = -1;                 // last_guess
    st->shared_int[3] = 0;                  // game_status running
    pthread_mutex_unlock(&st->shared_mutex);

    // 3) Start logger thread
    pthread_t log_tid;
    pthread_create(&log_tid, nullptr, loggerThread, nullptr);

    // 4) Start RR scheduler thread
    SchedulerArgs schedArgs;
    schedArgs.st = st;
    schedArgs.quantum_ms = 800; // change the quantum here

    pthread_t sched_tid;
    pthread_create(&sched_tid, nullptr, roundRobinThread, &schedArgs);

    // ---- Demo actions (simulate disconnects / guesses) ----
    logPush("[MAIN] Demo started. Watch game.log.");

    sleep(2);
    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[1] &= ~(1 << 1); // disconnect player 1
    pthread_mutex_unlock(&st->shared_mutex);
    logPush("[MAIN] Player 1 disconnected.");

    sleep(2);
    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[2] = 42; // last_guess updated (example)
    pthread_mutex_unlock(&st->shared_mutex);
    logPush("[MAIN] last_guess set to 42.");

    sleep(2);
    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[1] &= ~(1 << 0); // disconnect player 0
    pthread_mutex_unlock(&st->shared_mutex);
    logPush("[MAIN] Player 0 disconnected (scheduler should skip).");

    sleep(2);
    pthread_mutex_lock(&st->shared_mutex);
    st->shared_int[3] = 1; // end game
    pthread_mutex_unlock(&st->shared_mutex);
    logPush("[MAIN] Game ended.");

    st->shared_int[3] = 1;

    // 5) Wait scheduler to stop
    pthread_join(sched_tid, nullptr);

    // 6) Stop logger cleanly
    pthread_mutex_lock(&log_mutex);
    logger_running = false;
    pthread_cond_signal(&log_cv);
    pthread_mutex_unlock(&log_mutex);

    pthread_join(log_tid, nullptr);

    // Cleanup shared memory (demo)
  //  munmap(st, sizeof(SharedState));
 //   shm_unlink(SHM_NAME);
    

// ===========================
// Real server accept loop
// ===========================
listen_fd = setup_server_socket();
logPush("[SERVER] Listening on port 5555.");

int next_player_id = 0;

while (1) {
    struct sockaddr_in cli{};
    socklen_t len = sizeof(cli);

    int client_fd = accept(listen_fd, (struct sockaddr*)&cli, &len);
    if (client_fd < 0) continue;

    pthread_mutex_lock(&st->shared_mutex);
if (next_player_id >= MAX_PLAYERS) {
    pthread_mutex_unlock(&st->shared_mutex);
    close(client_fd);
    continue;
}
int pid = next_player_id++;
st->shared_int[1] |= (1 << pid); // mark connected
pthread_mutex_unlock(&st->shared_mutex);


    logPush("[SERVER] Client connected -> player " + to_string(pid));

pid_t cpid = fork();
if (cpid < 0) {                     
    perror("fork");
    close(client_fd);
    continue;
}
if (cpid == 0) {
    g_state = st;                 
    client_process(client_fd, pid);
}

    close(client_fd); // parent
}


    return 0;
}