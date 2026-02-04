#define _POSIX_C_SOURCE 200809L

#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <cstdlib>

using namespace std;

struct SharedState {
    pthread_mutex_t shared_mutex;
    int shared_int[4];
};

static const char* SHM_NAME = "/guess_game_shm_demo";

static void clearScreen() { system("clear"); }

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cout << "Usage: ./client <player_id>\n";
        return 1;
    }

    int player_id = atoi(argv[1]);
    string fifo = "/tmp/guess_game_client_" + to_string(player_id);

    // attach shared memory
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }

    SharedState* st = (SharedState*)mmap(nullptr, sizeof(SharedState),
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         shm_fd, 0);
    close(shm_fd);

    if (st == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    cout << "Player " << player_id << " started.\n";

    int last_state = -1;  
    while (true) {
        // 1) wait for my turn using shared memory
        bool shown = false;
        while (true) {
          pthread_mutex_lock(&st->shared_mutex);
          int current_player = st->shared_int[0];
          int game_status    = st->shared_int[3];
          pthread_mutex_unlock(&st->shared_mutex);

          if (game_status != 0) {
              
              cout << "Game ended.\n";
              munmap(st, sizeof(SharedState));
              return 0;
          }

          if (current_player == player_id) {
              if (last_state != 1) {   // ⭐ only once when entering YOUR TURN
            
                  cout << ">>> YOUR TURN! <<<\n";
                  cout << "Input your guess (1-100): ";
                  last_state = 1;
              }
              break;
          } else {
              if (last_state != 0) {   // ⭐ only once when entering WAITING
            
                  cout << ">>> Waiting for opponent... <<<\n";
                  last_state = 0;
              }
          }

          usleep(150 * 1000);
       }

        int guess;
        cin >> guess;
        if (cin.fail() || guess < 1 || guess > 100) {
            cin.clear();
            cin.ignore(1000, '\n');
            cout << "Invalid input. Please enter 1-100.\n";
            sleep(1);
            continue;
        }

        // 3) send guess to server (write to my FIFO)
        string msg = "GUESS " + to_string(player_id) + " " + to_string(guess);

        int fdw = open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        if (fdw < 0) {
            perror("open write fifo");
            sleep(1);
            continue;
        }
        write(fdw, msg.c_str(), msg.size() + 1);
        close(fdw);

        // 4) read response from same FIFO
        char buf[256] = {0};
        int fdr = open(fifo.c_str(), O_RDONLY); // blocking until server replies
        if (fdr < 0) {
            perror("open read fifo");
            sleep(1);
            continue;
        }
        ssize_t n = read(fdr, buf, sizeof(buf) - 1);
        close(fdr);

        if (n > 0) {
            cout << "Server: " << buf << "\n";
            if (strstr(buf, "WIN") != nullptr) {
                cout << "You won!\n";
                break;
            }
        }

        sleep(1);
    }

    munmap(st, sizeof(SharedState));
    return 0;
}
