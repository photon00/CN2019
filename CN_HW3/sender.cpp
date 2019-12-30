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

#define STATE_SEND   0
#define STATE_FULL   1
#define STATE_WAIT   2
#define STATE_RESEND 3

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

static int send_message(char* buf, int buf_len);
static void send_fin();

struct sockaddr_in sender, agent, tmp_addr;
int tmp_size, tmp_flag;
char hostname[512];
int socket_fd, sender_port;
cv::VideoCapture cap;
cv::Mat imgFrame;
char buf[512];

int winSize = 1, threshold = 16;
int segment_idx = 0, base = 0, num_ack = 0;
segment Segments[SEGMENT_LEN], s_tmp;

int main(int argc, char* argv[]){
	using namespace cv;
	if (argc != 5){
		fprintf(stderr, "Usage: %s [port] [agent ip] [agent port] [video filename]\n", argv[0]);
		exit(1);
	}
	if (cap.open(argv[4]) == false){
		ERR_EXIT("");
	}

	init_server((unsigned int) atoi(argv[1]));

	agent.sin_family = AF_INET;
    agent.sin_addr.s_addr = inet_addr(argv[2]);
    agent.sin_port = htons(atoi(argv[3]));
    memset(agent.sin_zero, '\0', sizeof(agent.sin_zero));
    
	int height = cap.get(CV_CAP_PROP_FRAME_HEIGHT);
	int width = cap.get(CV_CAP_PROP_FRAME_WIDTH);
	buf[0] = height & 0xff;
	buf[1] = (height >> 8) & 0xff;
	buf[2] = (height >> 16) & 0xff;
	buf[3] = (height >> 24) & 0xff;
	
	buf[4] = width & 0xff;
	buf[5] = (width >> 8) & 0xff;
	buf[6] = (width >> 16) & 0xff;
	buf[7] = (width >> 24) & 0xff;
	send_message(buf, 8);
	
	imgFrame = Mat::zeros(height, width, CV_8UC3);
	if(!imgFrame.isContinuous()){
         imgFrame = imgFrame.clone();
    }

	int imgSize = imgFrame.total() * imgFrame.elemSize();
    uchar buffer[imgSize];

    while (cap.read(imgFrame)){
    	memcpy(buffer, imgFrame.data, imgSize);
    	send_message((char*)buffer, imgSize);
    }
    cap.release();
    destroyAllWindows();
    send_fin();
	return 0;
}

static void init_server(unsigned short port) {
    struct timeval tv = {0, 10000};  // {sec, usec}
    int tmp = 1;

    gethostname(hostname, sizeof(hostname));
    fprintf(stderr, "Sender ip: %s\n", hostname);
    sender_port = port;

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) ERR_EXIT("socket");

    bzero(&sender, sizeof(sender));
    sender.sin_family = AF_INET;
    sender.sin_addr.s_addr = htonl(INADDR_ANY);
    sender.sin_port = htons(port);
    
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&tmp, sizeof(tmp)) < 0) {
        ERR_EXIT("setsockopt");
    }
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ERR_EXIT("setsockopt timeout");
    }
    if (bind(socket_fd, (struct sockaddr*)&sender, sizeof(sender)) < 0) {
        ERR_EXIT("bind");
    }
}

static int send_message(char* buf, int buf_len){
	int num_segment = buf_len % SEGMENT_SIZE == 0 ? buf_len/SEGMENT_SIZE : buf_len/SEGMENT_SIZE+1;
	int state = STATE_SEND;
	while (1){
		if (state == STATE_SEND){
			if (num_segment == 0) {state = STATE_WAIT; continue; }
			memset(&Segments[segment_idx%SEGMENT_LEN], 0, sizeof(segment));
			if (buf_len < SEGMENT_SIZE){
				memcpy(Segments[segment_idx%SEGMENT_LEN].data, buf, buf_len);
				Segments[segment_idx%SEGMENT_LEN].head.length = buf_len;
				buf_len -= buf_len;
				buf += buf_len;

			}
			else {
				memcpy(Segments[segment_idx%SEGMENT_LEN].data, buf, SEGMENT_SIZE);
				Segments[segment_idx%SEGMENT_LEN].head.length = SEGMENT_SIZE;
				buf_len -= SEGMENT_SIZE;
				buf += SEGMENT_SIZE;
			}
			Segments[segment_idx%SEGMENT_LEN].head.seqNumber = segment_idx;
			sendto(socket_fd, &Segments[segment_idx%SEGMENT_LEN], sizeof(segment), 0, (struct sockaddr *)&agent, sizeof(agent));
			printf("[S]send    data    #%d,    winSize = %d\n", segment_idx, winSize);
			segment_idx++;
			num_segment--;
			if (segment_idx - base == winSize && num_segment > 0) state = STATE_FULL;
		}
		else if (state == STATE_FULL){
			tmp_size = sizeof(tmp_addr);
			recvfrom(socket_fd, &s_tmp, sizeof(s_tmp), 0, (struct sockaddr *)&tmp_addr, (socklen_t*)&tmp_size);
			if (errno == EAGAIN){
				threshold = MAX(1, winSize/2);
				printf("[F]timeout                threshold = %d\n", threshold);
				errno = 0;
				state = STATE_RESEND;
				num_ack = 0;
			}
			else if (s_tmp.head.ack){
				printf("[F]recv    ack     #%d,\n", s_tmp.head.ackNumber);
				if (s_tmp.head.ackNumber >= base){
					num_ack++;
					base = s_tmp.head.ackNumber+1;
					state = STATE_SEND;
				}
				if (num_ack == winSize){
					num_ack = 0;
					winSize = winSize < threshold ? 2*winSize : winSize+1;
					winSize = MIN(winSize, SEGMENT_LEN);
				}
			}				
		}
		else if (state == STATE_WAIT) {
			while (base < segment_idx){
				tmp_size = sizeof(tmp_addr);
				recvfrom(socket_fd, &s_tmp, sizeof(s_tmp), 0, (struct sockaddr *)&tmp_addr, (socklen_t*)&tmp_size);
				if (errno == EAGAIN){
					threshold = MAX(1, winSize/2);
					printf("[W]timeout                threshold = %d\n", threshold);
					errno = 0;
					state = STATE_RESEND;
					num_ack = 0;
					break;
				}
				else if (s_tmp.head.ack){
					printf("[W]recv    ack     #%d,\n", s_tmp.head.ackNumber);
					if (s_tmp.head.ackNumber >= base){
						base = s_tmp.head.ackNumber+1;
						num_ack++;
					}
					if (num_ack == winSize){
						num_ack = 0;
						winSize = winSize < threshold ? 2*winSize : winSize+1;
						winSize = MIN(winSize, SEGMENT_LEN);
					}
				}				
			}
			if (base == segment_idx) break;
		}
		else {  // state == STATE_RESEND
			tmp_flag = winSize = 1;
			while (base+winSize <= segment_idx){
				printf("[R]resnd   data    #%d,    winSize = %d\n", base, winSize);
				sendto(socket_fd, &Segments[base%SEGMENT_LEN], sizeof(segment), 0, (struct sockaddr *)&agent, sizeof(agent));

				tmp_size = sizeof(tmp_addr);
				recvfrom(socket_fd, &s_tmp, sizeof(s_tmp), 0, (struct sockaddr *)&tmp_addr, (socklen_t*)&tmp_size);
				if (errno == EAGAIN){
					threshold = MAX(1, winSize/2);
					printf("[R]timeout                threshold = %d\n", threshold);
					errno = 0;
					tmp_flag = 0;
					break;
				}
				else if (s_tmp.head.ack){
					printf("[R]recv    ack     #%d,\n", s_tmp.head.ackNumber);
					if (s_tmp.head.ackNumber >= base){
						base = s_tmp.head.ackNumber+1;
					}
					else { tmp_flag = 0; break; }
				}				
			}
			if (tmp_flag) state = STATE_SEND;
		}
		fflush(stdout);
	}
}

static void send_fin(){
	while (1){
		s_tmp.head.seqNumber = segment_idx;
		s_tmp.head.length = 0;
		s_tmp.head.fin = 1;
		sendto(socket_fd, &s_tmp, sizeof(segment), 0, (struct sockaddr *)&agent, sizeof(agent));
		printf("send    fin\n"); fflush(stdout);
		tmp_size = sizeof(tmp_addr);
		recvfrom(socket_fd, &s_tmp, sizeof(s_tmp), 0, (struct sockaddr *)&tmp_addr, (socklen_t*)&tmp_size);
		if (errno == EAGAIN){
			printf("timeout\n"); fflush(stdout);
		}
		else if (s_tmp.head.ack){
			printf("recv    ackfin\n"); fflush(stdout);
			break;
		}
	}
}