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
#include "opencv2/opencv.hpp"

#define ERR_EXIT(a) { perror(a); exit(1); }
#define BUFF_SIZE 1024

typedef struct {
    char hostname[512];  // server's hostname
    unsigned short port;  // port to listen
    int listen_fd;  // fd to wait for a new connection
} server;

typedef struct {
    char host[512];  // client's host
    int conn_fd;  // fd to talk with client

    char filename[512];
    char buf[BUFF_SIZE];  // data sent by/to client
    size_t buf_len;  // bytes used by buf
	int open_fd;  // fd for put/get/play command
	int state;
	/*
	0 -> wait for command
	1 -> list files
	2 -> put file to server
	3 -> get file from server
	4 -> play file in server
	*/
	cv::Mat imgFrame;
	cv::VideoCapture *cap;
} request;

#define STATE_WTCMD 0
#define STATE_LIST  1
#define STATE_PUT   2
#define STATE_GET   3
#define STATE_PLAY  4

static void init_server(unsigned short port);
// initailize a server, exit for error

static void init_request(request* reqP);
// initailize a request instance

static void free_request(request* reqP);
// free resources used by a request instance

static int handle_read(request* reqP);
// read data to request[*].buf

static int handle_write(request *reqP);
// send data

static int decode_command(request *reqP);
// :return: 0 -> list files
//			1 -> quit
//			2 -> put file
// 			3 -> get file
//			4 -> play video

#define CMD_LIST 0
#define CMD_QUIT 1
#define CMD_PUT  2
#define CMD_GET  3
#define CMD_PLAY 4

static int send_file(request *reqP);

int getFilesize(int fd);

static int send_frame(request *reqP);

static void* thread_service(void* arg);

server svr;  // server
request* requestP = NULL;  // point to a list of requests
int maxfd;  // size of open file descriptor table, size of request list

using namespace std;
using namespace cv;
int main(int argc, char** argv){
	signal(SIGPIPE, SIG_IGN);
    int i, ret;

    struct sockaddr_in cliaddr;  // used by accept()
    int clilen;

    int conn_fd;  // fd for a new connection with client
    pthread_t pid;


    // Parse args.
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        exit(1);
    }

    // Initialize server
    init_server((unsigned short) atoi(argv[1]));

    // Get file descripter table size and initize request table
    maxfd = getdtablesize();
    requestP = (request*) malloc(sizeof(request) * maxfd);
    if (requestP == NULL) {
        ERR_EXIT("out of memory allocating all requests");
    }
    for (i = 0; i < maxfd; i++) {
        init_request(&requestP[i]);
    }
    requestP[svr.listen_fd].conn_fd = svr.listen_fd;
    strcpy(requestP[svr.listen_fd].host, svr.hostname);

    // Loop for handling connections
    fprintf(stderr, "\nstarting on %.80s, port %d, fd %d, maxconn %d...\n", svr.hostname, svr.port, svr.listen_fd, maxfd);

    while(true){   

        // Check new connection
        clilen = sizeof(cliaddr);
        conn_fd = accept(svr.listen_fd, (struct sockaddr*)&cliaddr, (socklen_t*)&clilen);
        if (conn_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;  // try again
            if (errno == ENFILE) {
                (void) fprintf(stderr, "out of file descriptor table ... (maxconn %d)\n", maxfd);
                continue;
            }
            ERR_EXIT("accept")
        }
        requestP[conn_fd].conn_fd = conn_fd;
        strcpy(requestP[conn_fd].host, inet_ntoa(cliaddr.sin_addr));
        fprintf(stderr, "[+] getting a new request... fd %d from %s\n", conn_fd, requestP[conn_fd].host);
        pthread_create(&pid, NULL, thread_service, &conn_fd);

    }
    free(requestP);
    return 0;
}

static void init_request(request* reqP) {
    reqP->conn_fd = -1;
    reqP->buf_len = 0;
    reqP->open_fd = -1;
    reqP->state = 0;
}

static void free_request(request* reqP) {
    init_request(reqP);
}


static void init_server(unsigned short port) {
    struct sockaddr_in servaddr;
    int tmp;

    gethostname(svr.hostname, sizeof(svr.hostname));
    svr.port = port;

    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0) ERR_EXIT("socket");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    tmp = 1;
    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&tmp, sizeof(tmp)) < 0) {
        ERR_EXIT("setsockopt");
    }
    if (bind(svr.listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, 1024) < 0) {
        ERR_EXIT("listen");
    }
}

// return 0: socket ended, request done.
// return 1: success, message (without header) got this time is in reqP->buf with reqP->buf_len bytes. read more until got <= 0.
// It's guaranteed that the header would be correctly set after the first read.
// error code:
// -1: client connection error
static int handle_read(request* reqP) {
    int r, end_flag=0, remain_bytes=4;
    char buf[BUFF_SIZE], *p;

    // Read in request from client
    // First get the number of bytes to receive
    p = buf;
    while (remain_bytes > 0){
    	r = read(reqP->conn_fd, p, remain_bytes);
    	if (r == 0){ end_flag = 1; break; }
    	remain_bytes -= r;
    	p += r;
    }
    if (end_flag) return 0;

    int num_bytes = *(int*)buf;
    //fprintf(stderr, "Attempt to recieve %d bytes from fd %d\n", num_bytes, reqP->conn_fd);

    // Second get the bytes according to the num_bytes
    p = buf, remain_bytes = num_bytes;
    while (remain_bytes > 0){
    	r = read(reqP->conn_fd, p, remain_bytes);
    	remain_bytes -= r;
    	p += r;
    }

	memmove(reqP->buf, buf, (size_t)num_bytes);
	reqP->buf[num_bytes] = '\0';
	reqP->buf_len = (size_t)num_bytes;
    return 1;
}


static int decode_command(request *reqP){
	int cmd = reqP->buf[0];
	if (cmd > 1)
		strcpy(reqP->filename, &reqP->buf[1]);
	return cmd;
}

static int handle_write(request *reqP){
	int r;
	char tmp[4];
	tmp[0] = reqP->buf_len & 0xff;
	tmp[1] = (reqP->buf_len >> 8) & 0xff;
	tmp[2] = (reqP->buf_len >> 16) & 0xff;
	tmp[3] = (reqP->buf_len >> 24) & 0xff;

	r = write(reqP->conn_fd, tmp, 4);
	if (r < 0) { 
		fprintf(stderr, "[-] Write number of bytes at fd %d: %s\n", reqP->conn_fd, strerror(errno));
		close(reqP->conn_fd);
		return -1;
	}

	r = write(reqP->conn_fd, reqP->buf, reqP->buf_len);
	if (r < 0) {
		fprintf(stderr, "[-] Write data at fd %d: %s\n", reqP->conn_fd, strerror(errno));
		close(reqP->conn_fd);
		return -1;
	}
}

static int send_file(request *reqP){
	int fd = reqP->open_fd;
	int file_size = getFilesize(fd);
	// tell server how many bytes would send
	reqP->buf[0] = file_size & 0xff;
	reqP->buf[1] = (file_size >> 8) & 0xff;
	reqP->buf[2] = (file_size >> 16) & 0xff;
	reqP->buf[3] = (file_size >> 24) & 0xff;
	reqP->buf_len = 4;
	if (handle_write(reqP) < 0) return -1;
	fprintf(stderr, "Sending file \"%s\" (%d bytes)\n", reqP->filename, file_size);

	// transfer file content
	int read_bytes;
	while ((read_bytes = read(fd, reqP->buf, BUFF_SIZE)) > 0){
		reqP->buf_len = read_bytes;
		if (handle_write(reqP) < 0) return -1;
	}
	return 0;
}

int getFilesize(int fd){
	off_t currentPos = lseek(fd, 0, SEEK_CUR);
	off_t file_size = lseek(fd, 0, SEEK_END);
	lseek(fd, currentPos, SEEK_SET);
	return (int)file_size;
}

static int send_frame(request *reqP){
	int height = reqP->cap->get(CV_CAP_PROP_FRAME_HEIGHT);
	int width = reqP->cap->get(CV_CAP_PROP_FRAME_WIDTH);
	reqP->buf[0] = height & 0xff;
	reqP->buf[1] = (height >> 8) & 0xff;
	reqP->buf[2] = (height >> 16) & 0xff;
	reqP->buf[3] = (height >> 24) & 0xff;
	
	reqP->buf[4] = width & 0xff;
	reqP->buf[5] = (width >> 8) & 0xff;
	reqP->buf[6] = (width >> 16) & 0xff;
	reqP->buf[7] = (width >> 24) & 0xff;
	reqP->buf_len = 8;
	if (handle_write(reqP) < 0) return -1;
	fprintf(stderr, "Playing file \"%s\" (%d*%d)\n", reqP->filename, width, height);

	// transfer frames
	reqP->imgFrame = Mat::zeros(height, width, CV_8UC3);
	if(!reqP->imgFrame.isContinuous()){
         reqP->imgFrame = reqP->imgFrame.clone();
    }
    
    int imgSize = reqP->imgFrame.total() * reqP->imgFrame.elemSize();
    uchar buffer[imgSize];

    int sent_bytes;
    while (reqP->cap->read(reqP->imgFrame)){
    	// notify the client the video is still playing
    	reqP->buf[0] = 0x01;
    	reqP->buf_len = 1;
    	if (handle_write(reqP) < 0) return -1;

    	// send a frame
    	memcpy(buffer, reqP->imgFrame.data, imgSize);
    	sent_bytes = 0;
    	while (sent_bytes < imgSize){
    		if (sent_bytes + BUFF_SIZE <= imgSize){
    			memcpy(reqP->buf, buffer+sent_bytes, BUFF_SIZE);
    			reqP->buf_len = BUFF_SIZE;
    			sent_bytes += BUFF_SIZE;
    		}
    		else {
    			memcpy(reqP->buf, buffer+sent_bytes, imgSize-sent_bytes);
    			reqP->buf_len = imgSize-sent_bytes;
    			sent_bytes = imgSize;
    		}
    		if (handle_write(reqP) < 0) return -1;
    	}
    	if (handle_read(reqP) != 1) return -1;
    	if (reqP->buf[0] < 0) break;  // client press esc

    }
    // notify client the video go to the end
    reqP->buf[0] = 0xff;
	reqP->buf_len = 1;
	if (handle_write(reqP) < 0) return -1;
	
	return 0;
}

static void* thread_service(void* arg){
	int conn_fd = *(int*)arg;
	int ret;

	// handling request
    while (true){
    	if (requestP[conn_fd].state == STATE_WTCMD){
        	ret = handle_read(&requestP[conn_fd]); // parse data from client to requestP[conn_fd].buf
        	if (ret > 0){
        		int cmd = decode_command(&requestP[conn_fd]);
				fprintf(stderr, "Recieve command = %d", cmd);
				if (cmd > 1)
					fprintf(stderr, ", with filename: %s", requestP[conn_fd].filename);
				fprintf(stderr, "\n");
				switch (cmd){
					case CMD_LIST: requestP[conn_fd].state = STATE_LIST; break;
					case CMD_QUIT: continue;
					case CMD_PUT:  requestP[conn_fd].state = STATE_PUT; break;
					case CMD_GET:  requestP[conn_fd].state = STATE_GET; break;
					case CMD_PLAY: requestP[conn_fd].state = STATE_PLAY; break;
					default: continue;
				}
			}
			else {
				fprintf(stderr, "[-] Connection closed by client at fd %d\n", requestP[conn_fd].conn_fd);
				break;
			}
		}
		else if (requestP[conn_fd].state == STATE_LIST){
			DIR *d = opendir(".");
			struct dirent *dir;
			char *p = requestP[conn_fd].buf;
			requestP[conn_fd].buf_len = 0;
			if (d){
				while ((dir = readdir(d)) != NULL){
					sprintf(p, "%s\n", dir->d_name);
					p = p+strlen(dir->d_name)+1;
					requestP[conn_fd].buf_len += strlen(dir->d_name)+1;
				}
				closedir(d);
			}
			if (handle_write(&requestP[conn_fd]) < 0) break;
			requestP[conn_fd].state = STATE_WTCMD;
		}
		else if (requestP[conn_fd].state == STATE_PUT){
			if (handle_read(&requestP[conn_fd]) != 1) break;
			int file_size = *(int*)requestP[conn_fd].buf;
			fprintf(stderr, "Receiving file \"%s\" (%d bytes)\n", requestP[conn_fd].filename, file_size);
			requestP[conn_fd].open_fd = open(requestP[conn_fd].filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
			if (requestP[conn_fd].open_fd < 0){ perror(requestP[conn_fd].filename); break; }
			int received_bytes = 0;
			while (received_bytes < file_size){
				if (handle_read(&requestP[conn_fd]) != 1) break;
				write(requestP[conn_fd].open_fd, requestP[conn_fd].buf, requestP[conn_fd].buf_len);
				received_bytes += requestP[conn_fd].buf_len;
			}
			close(requestP[conn_fd].open_fd);
			requestP[conn_fd].open_fd = -1;
			requestP[conn_fd].state = STATE_WTCMD;
		}
		else if (requestP[conn_fd].state == STATE_GET){
			requestP[conn_fd].open_fd = open(requestP[conn_fd].filename, O_RDONLY);
			if (requestP[conn_fd].open_fd < 0) {
				perror(requestP[conn_fd].filename);
        		requestP[conn_fd].buf[0] = -errno & 0xff;
        		requestP[conn_fd].buf[1] = (-errno >> 8) & 0xff;
        		requestP[conn_fd].buf[2] = (-errno >> 16) & 0xff;
        		requestP[conn_fd].buf[3] = (-errno >> 24) & 0xff;
        		requestP[conn_fd].buf_len = 4;
        		if (handle_write(&requestP[conn_fd]) < 0) { close(requestP[conn_fd].open_fd); break; }
        		requestP[conn_fd].state = STATE_WTCMD;
        		continue;
        	}
        	if (send_file(&requestP[conn_fd]) < 0){ close(requestP[conn_fd].open_fd); break; }
			close(requestP[conn_fd].open_fd);
			requestP[conn_fd].open_fd = -1;
			requestP[conn_fd].state = STATE_WTCMD;
		}
		else {  // STATE_PLAY
			requestP[conn_fd].cap = new VideoCapture();
			if (requestP[conn_fd].cap->open(requestP[conn_fd].filename) == false){
				requestP[conn_fd].buf[0] = 0xff;
				requestP[conn_fd].buf[1] = 0xff;
				requestP[conn_fd].buf[2] = 0xff;
				requestP[conn_fd].buf[3] = 0xff;
				requestP[conn_fd].buf_len = 4;
        		if (handle_write(&requestP[conn_fd]) < 0) { requestP[conn_fd].cap->release(); break; }
				requestP[conn_fd].state = STATE_WTCMD;
				continue;
			}
        	if (send_frame(&requestP[conn_fd]) < 0){ requestP[conn_fd].cap->release(); break; }
        	requestP[conn_fd].cap->release();
        	delete(requestP[conn_fd].cap);
			requestP[conn_fd].state = STATE_WTCMD;
		}
		
	}

	// clean request
    close(requestP[conn_fd].conn_fd);
	free_request(&requestP[conn_fd]);
}