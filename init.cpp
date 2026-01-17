#include <iostream>
using namespace std;
//called once in parent
void init_sync(SharedState* state) {
    // ---- mutex attributes ----
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

    // ---- init mutex ----
    pthread_mutex_init(&state->game_mutex, &attr);

    // ---- init semaphores ----
    for (int i = 0; i < MAX_PLAYERS; i++) {
        sem_init(&state->turn_sem[i], 1, 0); // pshared=1, initial=0
        state->active[i] = 0;
    }

    // ---- initial game values ----
    pthread_mutex_lock(&state->game_mutex);
    state->game_state = WAITING;
    state->current_turn = 0;
    state->winner = -1;
    pthread_mutex_unlock(&state->game_mutex);
}