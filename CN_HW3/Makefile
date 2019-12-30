CC = g++
OPENCV =  `pkg-config --cflags --libs opencv`
DEBUG = -O3

SENDER = sender.cpp
RECEIVER = receiver.cpp
AGENT = agent.c

SDR = sender
REV = receiver
AGT = agent

all: agent sender receiver
  
sender: $(SENDER)
	$(CC) $(SENDER) -o $(SDR)  $(OPENCV) $(DEBUG)
receiver: $(RECEIVER)
	$(CC) $(RECEIVER) -o $(REV)  $(OPENCV) $(DEBUG)
agent: $(AGENT)
	gcc $(AGENT) -o $(AGT)

.PHONY: clean

clean:
	rm $(REV) $(SDR) $(AGT)