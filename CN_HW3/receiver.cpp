#include <iostream>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <string.h>
#include <string>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include "opencv2/opencv.hpp"

#define ERR_EXIT(a) { perror(a); exit(1); }
//#define MAX(a, b) ((a) > (b) ? (a) : (b))
//#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define SEGMENT_LEN 200
#define SEGMENT_SIZE 10000

typedef struct {
	int length;
	int seqNumber;
	int ackNumber;
	int fin;
	int syn;
	int ack;
} header;

typedef struct{
	header head;
	char data[SEGMENT_SIZE];
} segment;


static void init_server(unsigned short port);
// initailize a server, exit for error

static void flush_buffer();
// merge data in Segments[] to buffer

static int read_message(char *buf, int length);
// read data from buffer

static int read_socket(int length);
// try to copy data from segments[] to buffer

struct sockaddr_in receiver, agent, tmp_addr;
int tmp_size;
char hostname[512];
int socket_fd, receiver_port;
cv::Mat imgFrame;
// socket buffer IO
char buffer[SEGMENT_LEN * SEGMENT_SIZE];
int buf_offset, buf_len;
int getfin;

segment Segments[SEGMENT_LEN], s_tmp;
int segment_len, segment_idx, num_seg;  // segment_len: sum of data length in Segments[]
										// segment_idx: seqNumber
										// num_seg:     number of segments in Segments[] waiting for flush

int main(int argc, char* argv[]){
	using namespace cv;
	int height, width;
	char tmp[32];

	if (argc != 4){
		fprintf(stderr, "Usage: %s [port] [agent ip] [agent port]\n", argv[0]);
		exit(1);
	}

	init_server((unsigned int) atoi(argv[1]));

	agent.sin_family = AF_INET;
    agent.sin_addr.s_addr = inet_addr(argv[2]);
    agent.sin_port = htons(atoi(argv[3]));
    memset(agent.sin_zero, '\0', sizeof(agent.sin_zero));
    
    read_message(tmp, 8);
    height = *(int*)tmp;
    width = *(int*)(tmp+4);
    fprintf(stderr, "get video size %d x %d\n", height, width);
	
	imgFrame = Mat::zeros(height, width, CV_8UC3);
	if(!imgFrame.isContinuous()){
         imgFrame = imgFrame.clone();
    }

	int imgSize = imgFrame.total() * imgFrame.elemSize();
    uchar imgBuffer[imgSize];

    while (1){
    	int ready_bytes = 0;
    	while (ready_bytes < imgSize){
    		ready_bytes += read_message((char*)(imgBuffer+ready_bytes), imgSize-ready_bytes);
    		if (getfin) break;    		
    	}
    	if (getfin) break;
    	memcpy(imgFrame.data, imgBuffer, imgSize);
    	imshow("play", imgFrame);
    	if (waitKey(3.33) == 27) break;
    }
    destroyAllWindows();
	return 0;
}

static void init_server(unsigned short port) {
	struct timeval tv = {1, 0};
    int tmp = 1;

    gethostname(hostname, sizeof(hostname));
    receiver_port = port;

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) ERR_EXIT("socket");

    bzero(&receiver, sizeof(receiver));
    receiver.sin_family = AF_INET;
    receiver.sin_addr.s_addr = htonl(INADDR_ANY);
    receiver.sin_port = htons(port);
    
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&tmp, sizeof(tmp)) < 0) {
        ERR_EXIT("setsockopt");
    }
    /*if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ERR_EXIT("setsockopt timeout");
    }*/
    if (bind(socket_fd, (struct sockaddr*)&receiver, sizeof(receiver)) < 0) {
        ERR_EXIT("bind");
    }
}

static void flush_buffer(){
	for (int i=0; i<num_seg; ++i){
		memcpy(buffer+buf_len, Segments[i].data, Segments[i].head.length);
		buf_len += Segments[i].head.length;
	}
	num_seg = segment_len = 0;
}

static int read_message(char *buf, int length){
	int read_length;
	if (length > buf_len - buf_offset){
		memcpy(buffer, buffer+buf_offset, buf_len-buf_offset);
		buf_len = buf_len-buf_offset;
		buf_offset = 0;
		read_length = read_socket(length-(buf_len - buf_offset));
		memcpy(buf, buffer+buf_offset, read_length);
		buf_offset += read_length;
		return read_length;
	}
	else {
		memcpy(buf, buffer+buf_offset, length);
		buf_offset += length;
		return length;
	}
}

static int read_socket(int length){
	while (length > segment_len && num_seg<SEGMENT_LEN){
		tmp_size = sizeof(tmp_addr);
		recvfrom(socket_fd, &s_tmp, sizeof(s_tmp), 0, (struct sockaddr *)&tmp_addr, (socklen_t*)&tmp_size);
		/*if (errno == EAGAIN){
			printf("read socket timeout\n");
			errno = 0;
			break;
		}*/
		if (s_tmp.head.seqNumber != segment_idx){
			printf("drop    data    #%d\n", s_tmp.head.seqNumber);
			printf("send    ack     #%d\n", segment_idx-1);
			s_tmp.head.ack = 1;
			s_tmp.head.length = 0;
			s_tmp.head.ackNumber = segment_idx-1;
			sendto(socket_fd, &s_tmp, sizeof(segment), 0, (struct sockaddr *)&agent, sizeof(agent));
		}
		else {
			if (s_tmp.head.fin == 1){
				printf("recv    fin\n"); 
				printf("send    finack\n");
				getfin = 1;
			}
			else {
				printf("recv    data    #%d\n", s_tmp.head.seqNumber);
				printf("send    ack     #%d\n", s_tmp.head.seqNumber);
			}
			segment_len += s_tmp.head.length;
			segment_idx++;
			memcpy(&Segments[num_seg++], &s_tmp, sizeof(segment));

			s_tmp.head.ack = 1;
			s_tmp.head.length = 0;
			s_tmp.head.ackNumber = s_tmp.head.seqNumber;
			sendto(socket_fd, &s_tmp, sizeof(segment), 0, (struct sockaddr *)&agent, sizeof(agent));
			if (getfin) break;
		}
	}
	int return_length = segment_len;
	flush_buffer();
	fflush(stdout);
	return return_length;
}