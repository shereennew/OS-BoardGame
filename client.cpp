// client_proper.cpp - WAITS PROPERLY FOR TURNS
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
    string my_fifo = "/tmp/guess_game_client_" + to_string(player_id);
    
    cout << "👤 Player " << player_id << endl;
    
    // Wait for server
    cout << "Connecting to server...";
    while (access(my_fifo.c_str(), F_OK) == -1) {
        sleep(1);
        cout << ".";
    }
    cout << "\n✅ Connected!" << endl;
    cout << "Game will start shortly..." << endl;
    
    sleep(2);  // Wait for game to initialize
    
    while (true) {
        // ===== STEP 1: ASK SERVER ONCE =====
        cout << "\n[?] Checking if it's my turn..." << endl;
        
        string ask = "ASK_TURN " + to_string(player_id);
        int fd = open(my_fifo.c_str(), O_WRONLY);
        if (fd > 0) {
            write(fd, ask.c_str(), ask.length() + 1);
            close(fd);
        }
        
        // ===== STEP 2: GET RESPONSE =====
        char response[256];
        memset(response, 0, sizeof(response));
        int fd_resp = open(my_fifo.c_str(), O_RDONLY);
        if (fd_resp > 0) {
            read(fd_resp, response, sizeof(response));
            close(fd_resp);
        }
        
        string resp_str = response;
        cout << "📡 Server: " << resp_str << endl;
        
        // ===== STEP 3: HANDLE RESPONSE =====
        if (resp_str.find("YES") != string::npos || 
            resp_str.find("YOUR_TURN") != string::npos) {
            
            // ===== IT'S MY TURN - GET GUESS =====
            cout << "\n═══════════════════════════════════════" << endl;
            cout << "              🎮 YOUR TURN! 🎮" << endl;
            cout << "═══════════════════════════════════════" << endl;
            
            while (true) {
                cout << "\nEnter guess (1-100): ";
                
                string input;
                cin >> input;
                
                // Check if valid number
                bool valid = true;
                for (char c : input) {
                    if (!isdigit(c)) {
                        valid = false;
                        break;
                    }
                }
                
                if (!valid) {
                    cout << "❌ Please enter a number." << endl;
                    continue;
                }
                
                int guess = stoi(input);
                
                if (guess < 1 || guess > 100) {
                    cout << "❌ Guess must be between 1-100." << endl;
                    continue;
                }
                
                // ===== SEND GUESS =====
                cout << "📤 Sending guess: " << guess << endl;
                
                string guess_msg = "GUESS " + to_string(player_id) + " " + to_string(guess);
                int fd_send = open(my_fifo.c_str(), O_WRONLY);
                if (fd_send > 0) {
                    write(fd_send, guess_msg.c_str(), guess_msg.length() + 1);
                    close(fd_send);
                }
                
                // ===== GET RESULT =====
                cout << "⏳ Waiting for result..." << endl;
                
                char result[256];
                memset(result, 0, sizeof(result));
                int fd_result = open(my_fifo.c_str(), O_RDONLY);
                if (fd_result > 0) {
                    read(fd_result, result, sizeof(result));
                    close(fd_result);
                }
                
                cout << "📡 Result: " << result << endl;
                
                if (strstr(result, "WIN") != nullptr) {
                    cout << "\n🎉🎉🎉 CONGRATULATIONS! YOU WON! 🎉🎉🎉" << endl;
                    return 0;
                }
                
                break;  // Exit guess loop
            }
            
            // ===== AFTER GUESSING, WAIT FOR NEXT TURN =====
            cout << "\n⏳ Turn completed. Waiting for next turn..." << endl;
            sleep(4);  // Wait 4 seconds before checking again
            
        } else {
            // ===== NOT MY TURN =====
            if (resp_str.find("NO") != string::npos || resp_str.find("Player") != string::npos) {
                cout << "⏸️  Not your turn yet. ";
                
                // Extract which player's turn it is
                size_t player_pos = resp_str.find("Player ");
                if (player_pos != string::npos) {
                    cout << "It's " << resp_str.substr(player_pos) << endl;
                }
            } else {
                cout << "⏸️  Waiting for turn..." << endl;
            }
            
            sleep(3);  // Wait 3 seconds before checking again
        }
    }

    munmap(st, sizeof(SharedState));
    return 0;
}
