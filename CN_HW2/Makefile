CC = g++
OPENCV =  `pkg-config --cflags --libs opencv`
PTHREAD = -pthread

CLIENT = client.cpp
SERVER = server.cpp
CLI = client
SER = server

all: server client
  
server: $(SERVER)
	$(CC) $(SERVER) -o $(SER)  $(OPENCV) $(PTHREAD) -g
client: $(CLIENT)
	$(CC) $(CLIENT) -o $(CLI)  $(OPENCV) $(PTHREAD) -g
	cp $(CLI) test_client/

test: opencv pthread

opencv: openCV.cpp
	$(CC) openCV.cpp -o opencv $(OPENCV) -g
pthread: pthread.cpp
	$(CC) pthread.cpp -o pthread $(PTHREAD) -g

.PHONY: clean

clean:
	rm $(CLI) $(SER)

remove:
	rm opencv pthread