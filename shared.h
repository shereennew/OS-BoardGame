#pragma once //ensure run once
#include <pthread.h>
#include <semaphore.h>

#define MAX_PLAYERS 3

enum GameState { //check game state with number
    WAITING = 0,
    RUNNING = 1,
    ENDED   = 2
};

struct SharedState {
    // game data
    int game_state;
    int current_turn;
    int winner;
    int active[MAX_PLAYERS];

    // synchronization
    pthread_mutex_t game_mutex;   // protects game data
    sem_t turn_sem[MAX_PLAYERS];  // one semaphore per player
};