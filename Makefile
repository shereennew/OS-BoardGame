all: server client

server: server.cpp
	g++ -pthread -std=c++11 -D_POSIX_C_SOURCE=200809L server.cpp -o server

client: client.cpp
	g++ -std=c++11 -D_POSIX_C_SOURCE=200809L client.cpp -o client

clean:
	rm -f server client game.log scores.txt /tmp/guess_game_*
