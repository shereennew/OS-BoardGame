// server.cpp
#include <iostream>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>

static constexpr int MAX_PLAYERS = 5;

enum GameState
{
    WAITING = 0,
    RUNNING = 1,
    ENDED = 2
};

struct SharedState
{
    // ----- shared game data -----
    int game_state;
    int current_turn;
    int winner;
    int active[MAX_PLAYERS];

    // ----- synchronization tools (lecture Week 6: mutex + semaphores) -----
    pthread_mutex_t game_mutex;  // protects game_state/current_turn/winner/active[]
    sem_t turn_sem[MAX_PLAYERS]; // one semaphore per player (turn control)
};

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int) { g_stop = 1; }

static void on_sigchld(int)
{
    while (waitpid(-1, nullptr, WNOHANG) > 0)
    {
    }
}

// -------- System V shared memory (no <sys/mman.h>) --------
static int g_shmid = -1;

SharedState *create_and_attach_shared()
{
    // You can choose any key, but keep it constant for your project
    key_t key = ftok(".", 65);
    if (key == -1)
    {
        perror("ftok");
        return nullptr;
    }

    g_shmid = shmget(key, sizeof(SharedState), IPC_CREAT | 0666);
    if (g_shmid < 0)
    {
        perror("shmget");
        return nullptr;
    }

    void *addr = shmat(g_shmid, nullptr, 0);
    if (addr == (void *)-1)
    {
        perror("shmat");
        return nullptr;
    }

    return reinterpret_cast<SharedState *>(addr);
}

void detach_and_destroy_shared(SharedState *st)
{
    if (st)
        shmdt(st);
    if (g_shmid >= 0)
        shmctl(g_shmid, IPC_RMID, nullptr);
}

// -------- Init synchronization (process-shared mutex + process-shared sem) --------
void init_sync(SharedState *st)
{
    // Clear memory first
    std::memset(st, 0, sizeof(SharedState));

    // 1) Mutex must be process-shared (works across fork)
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&st->game_mutex, &mattr);

    // 2) Semaphores must be process-shared too (pshared=1)
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        sem_init(&st->turn_sem[i], 1, 0); // shared between processes, start locked
        st->active[i] = 0;
    }

    // 3) Initial game values
    pthread_mutex_lock(&st->game_mutex);
    st->game_state = WAITING;
    st->current_turn = 0;
    st->winner = -1;
    pthread_mutex_unlock(&st->game_mutex);
}

// -------- Scheduler thread (Round Robin) --------
void *scheduler_thread(void *arg)
{
    auto *st = reinterpret_cast<SharedState *>(arg);

    while (!g_stop)
    {
        pthread_mutex_lock(&st->game_mutex);

        if (st->game_state != RUNNING || st->winner != -1)
        {
            pthread_mutex_unlock(&st->game_mutex);
            usleep(100000);
            continue;
        }

        // Find next active player (skip disconnected)
        int next = st->current_turn;
        int tries = 0;
        do
        {
            next = (next + 1) % MAX_PLAYERS;
            tries++;
        } while (tries <= MAX_PLAYERS && st->active[next] == 0);

        // If nobody active, just wait
        if (tries > MAX_PLAYERS)
        {
            pthread_mutex_unlock(&st->game_mutex);
            usleep(100000);
            continue;
        }

        st->current_turn = next;
        pthread_mutex_unlock(&st->game_mutex);

        // Give turn to that player (wake child)
        sem_post(&st->turn_sem[next]);

        usleep(100000);
    }

    return nullptr;
}

// -------- Child player loop (synchronization only; FIFO read/write is your other part) --------
int read_guess_from_client_stub(int /*playerId*/)
{
    // TODO: replace with FIFO read
    // For testing, just return a random-ish number
    return (int)(getpid() % 50) + 1;
}

void player_process(SharedState *st, int myId, int secret_number_demo)
{
    while (!g_stop)
    {
        // Wait until scheduler gives me turn
        sem_wait(&st->turn_sem[myId]);

        pthread_mutex_lock(&st->game_mutex);

        // If game ended or stopped, release lock and continue
        if (st->game_state != RUNNING || st->winner != -1 || st->active[myId] == 0)
        {
            pthread_mutex_unlock(&st->game_mutex);
            continue;
        }

        pthread_mutex_unlock(&st->game_mutex);

        // ---- it's my turn now ----
        int guess = read_guess_from_client_stub(myId);

        pthread_mutex_lock(&st->game_mutex);
        if (guess == secret_number_demo)
        {
            st->winner = myId;
            st->game_state = ENDED;
            std::cout << "[Child " << myId << "] guessed CORRECT (" << guess << ")\n";
        }
        else
        {
            std::cout << "[Child " << myId << "] guessed " << guess << "\n";
        }
        pthread_mutex_unlock(&st->game_mutex);
    }

    _exit(0);
}

int main()
{
    std::signal(SIGINT, on_sigint);
    std::signal(SIGCHLD, on_sigchld);

    SharedState *st = create_and_attach_shared();
    if (!st)
        return 1;

    init_sync(st);

    // Demo: mark 3 players active and start game
    pthread_mutex_lock(&st->game_mutex);
    st->active[0] = 1;
    st->active[1] = 1;
    st->active[2] = 1;
    st->game_state = RUNNING;
    st->current_turn = 0;
    st->winner = -1;
    pthread_mutex_unlock(&st->game_mutex);

    // Start scheduler thread (in parent)
    pthread_t sched;
    pthread_create(&sched, nullptr, scheduler_thread, st);

    // Fork children (in real project: fork when players join)
    int secret_number_demo = 17; // TODO: set real secret number in shared memory
    for (int i = 0; i < 3; i++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            player_process(st, i, secret_number_demo);
        }
    }

    // Parent waits until winner appears (demo)
    while (!g_stop)
    {
        pthread_mutex_lock(&st->game_mutex);
        int w = st->winner;
        pthread_mutex_unlock(&st->game_mutex);

        if (w != -1)
        {
            std::cout << "Winner is player " << w << "\n";
            break;
        }
        usleep(200000);
    }

    g_stop = 1;
    pthread_join(sched, nullptr);

    detach_and_destroy_shared(st);
    return 0;
}
