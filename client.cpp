#define _POSIX_C_SOURCE 200809L

#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

using namespace std;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cout << "Usage: ./client <player_id>" << endl;
        return 1;
    }
    
    int player_id = atoi(argv[1]);
    
    // Server FIFO (for sending)
    string server_fifo = "/tmp/guess_game_client_" + to_string(player_id);
    
    // Client FIFO (for receiving) - THE SERVER CREATES THIS
    // Client doesn't create it, just opens it
    
    cout << "Player " << player_id << " started." << endl;
    cout << "Enter guesses (1-100):" << endl;
    
    // Create server FIFO if doesn't exist
    mkfifo(server_fifo.c_str(), 0666);
    
    while (true) {
        // Get guess from user
        cout << "Guess (1-100): ";
        int guess;
        cin >> guess;
        
        if (cin.fail() || guess < 1 || guess > 100) {
            cout << "Invalid input. Try again." << endl;
            cin.clear();
            cin.ignore(1000, '\n');
            continue;
        }
        
        // Send to server
        string message = "GUESS " + to_string(player_id) + " " + to_string(guess);
        
        int fd = open(server_fifo.c_str(), O_WRONLY);
        if (fd < 0) {
            cout << "Cannot connect to server!" << endl;
            sleep(1);
            continue;
        }
        
        write(fd, message.c_str(), message.length() + 1);
        close(fd);
        
        // Wait for response
        string client_fifo = "/tmp/guess_game_client_" + to_string(player_id);
        char buffer[256];
        int fd_resp = open(client_fifo.c_str(), O_RDONLY);
        if (fd_resp > 0) {
            read(fd_resp, buffer, sizeof(buffer));
            cout << "Server: " << buffer << endl;
            close(fd_resp);
            
            if (strstr(buffer, "WIN") != nullptr) {
                cout << "You won the game!" << endl;
                break;
            }
        }
        
        sleep(1);
    }
    
    return 0;
}