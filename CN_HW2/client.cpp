#include <iostream>
#include <sys/socket.h> 
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include "opencv2/opencv.hpp"

#define ERR_EXIT(a) { perror(a); exit(1); }
#define BUFF_SIZE 1024

typedef struct{
	int socket_fd;  // socket fd for client
	char hostname[512];  // hostname fo the server trying to connect
	char buf[BUFF_SIZE]; // buffer for message exchange
	char filename[512];
	size_t buf_len;

	cv::Mat imgFrame;
} client;

static void init_client(char ip[], unsigned short port);

static void handle_write();

static int handle_read();

static void send_command(int cmd);

static int decode_command(char command[]);
// :return: 0 -> list files
//			1 -> quit
//			2 -> put file
// 			3 -> get file
//			4 -> play video
//			-1 -> command error

static void send_file(int fd);

int getFilesize(int fd);

#define CMD_LIST 0
#define CMD_QUIT 1
#define CMD_PUT  2
#define CMD_GET  3
#define CMD_PLAY 4
#define CMD_ERR -1
#define CMD_FMT -2

client clt;
char terminal_buf[BUFF_SIZE];

using namespace std;
using namespace cv;
int main(int argc , char *argv[])
{
	signal(SIGPIPE, SIG_IGN);
	int cmd, ret;
	if (argc != 2){
		fprintf(stderr, "usage: %s [ip:port]\n", argv[0]);
		exit(1);
	}
    char *p = strchr(argv[1], ':');
    *p = '\0';

	init_client(argv[1], (unsigned short) atoi(p+1));

    while (true){
        printf("> ");
        fgets(terminal_buf, BUFF_SIZE, stdin);
        cmd = decode_command(terminal_buf);
        if (cmd == CMD_ERR){
        	fprintf(stderr, "Command not found\n");
        	continue;
        }
        if (cmd == CMD_FMT){
        	fprintf(stderr, "Command format error\n");
        	continue;
        }
        if (cmd == CMD_QUIT){ send_command(cmd); break; }
        if (cmd == CMD_LIST){
        	send_command(cmd);
        	if (handle_read() != 1) ERR_EXIT("socket read");
        	fprintf(stderr, "%s\n", clt.buf);
        }
        else if (cmd == CMD_PUT){
        	int open_fd = open(clt.filename, O_RDONLY);
        	if (open_fd < 0) {
        		perror(clt.filename);
        		continue;
        	}
        	send_command(cmd);
        	send_file(open_fd);
        	close(open_fd);
        }
        else if (cmd == CMD_GET){
        	send_command(cmd);
        	if (handle_read() != 1) ERR_EXIT("socket read");
        	int file_size = *(int*)clt.buf;
        	if (file_size < 0){
        		errno = -file_size;
        		perror(clt.filename);
        		continue;
        	}

        	int open_fd = open(clt.filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        	if (open_fd < 0){ perror(clt.filename); continue; }
        	
        	char print_buf[50];
        	printf("Receiving file \"%s\" ", clt.filename);
        	sprintf(print_buf, "(0/%d bytes)", file_size);
        	printf("%s", print_buf);

        	int received_bytes = 0;
			while (received_bytes < file_size){
				if (handle_read() != 1) ERR_EXIT("socket read");
				write(open_fd, clt.buf, clt.buf_len);
				received_bytes += clt.buf_len;
				for (int i=0; i<strlen(print_buf); ++i) printf("\b");
				fflush(stdin);
				sprintf(print_buf, "(%d/%d bytes)", received_bytes, file_size);
        		printf("%s", print_buf);
			}
			printf("\n");
			close(open_fd);
        }
        else if (cmd == CMD_PLAY){
        	char *p = strchr(clt.filename, '.');
        	if (p == NULL || strcmp(p, ".mpg") != 0){
        		fprintf(stderr, "The \"%s\" is not a mpg file\n", clt.filename);
        		continue;
        	}
        	send_command(cmd);
			if (handle_read() != 1) ERR_EXIT("socket read");
			int height = *(int*)clt.buf;
			if (height < 0){
				fprintf(stderr, "Can not play file \"%s\": %s\n", clt.filename, strerror(-height));
				continue;
			}
			int width = *(int*)(clt.buf+4);
			clt.imgFrame = Mat::zeros(height, width, CV_8UC3);
			int imgSize = clt.imgFrame.total() * clt.imgFrame.elemSize();
			uchar buffer[imgSize];
			fprintf(stderr, "playing video \"%s\" (%d*%d)\n", clt.filename, width, height);

			int received_bytes;
			if (handle_read() != 1) ERR_EXIT("socket read");
			char flag = clt.buf[0];
			while (flag >= 0){
				received_bytes = 0;
				while (received_bytes < imgSize){
					if (handle_read() != 1) ERR_EXIT("socket read");
					memcpy(buffer+received_bytes, clt.buf, clt.buf_len);
					received_bytes += clt.buf_len;
				}
				memcpy(clt.imgFrame.data, buffer, imgSize);
				imshow(clt.filename, clt.imgFrame);
				if (waitKey(3.33) == 27){  // press esc stop playing
					clt.buf[0] = 0xff;
					clt.buf_len = 1;
					handle_write();
				}
				else {
					clt.buf[0] = 0x01;
					clt.buf_len = 1;
					handle_write();
				}
				// get flag
				if (handle_read() != 1) ERR_EXIT("socket read");
				flag = clt.buf[0];
			}
			destroyAllWindows();
        }
    }
    fprintf(stderr, "close socket, exit\n");
    close(clt.socket_fd);
    return 0;
}


static void init_client(char ip[], unsigned short port){
    strcpy(clt.hostname, ip);
	clt.buf_len = 0;
	clt.socket_fd = socket(AF_INET , SOCK_STREAM , 0);

    if (clt.socket_fd == -1){
        ERR_EXIT("Create Socket");
    }

    struct sockaddr_in info;
    bzero(&info, sizeof(info));

    info.sin_family = PF_INET;
    info.sin_addr.s_addr = inet_addr(clt.hostname);
    info.sin_port = htons(port);


    int err = connect(clt.socket_fd,(struct sockaddr *)&info,sizeof(info));
    if (err == -1){
        ERR_EXIT("Connection");
    }

}

static void handle_write(){
	int r;
	char tmp[4];
	tmp[0] = clt.buf_len & 0xff;
	tmp[1] = (clt.buf_len >> 8) & 0xff;
	tmp[2] = (clt.buf_len >> 16) & 0xff;
	tmp[3] = (clt.buf_len >> 24) & 0xff;

	r = write(clt.socket_fd, tmp, 4);
	if (r < 0) ERR_EXIT("Write number of bytes");

	r = write(clt.socket_fd, clt.buf, clt.buf_len);
	if (r < 0) ERR_EXIT("Write data");
}

static int handle_read(){
    int r, end_flag=0, remain_bytes=4;
    char buf[BUFF_SIZE], *p;

    // Read in request from client
    // First get the number of bytes to receive
    p = buf;
    while (remain_bytes > 0){
    	r = read(clt.socket_fd, p, remain_bytes);
    	if (r == 0){ end_flag = 1; break; }
    	remain_bytes -= r;
    	p += r;
    }
    if (end_flag) return 0;
    int num_bytes = *(int*)buf;

    // Second get the bytes according to the num_bytes
    p = buf, remain_bytes = num_bytes;
    while (remain_bytes > 0){
    	r = read(clt.socket_fd, p, remain_bytes);
    	remain_bytes -= r;
    	p += r;
    }

	memmove(clt.buf, buf, (size_t)num_bytes);
	clt.buf[num_bytes] = '\0';
	clt.buf_len = (size_t)num_bytes;
    return 1;
}

static int decode_command(char message[]){
	char *h0=message, *e0, *h1, *e1;
	char *str_end = message+strlen(message);
	while (isspace(*h0)) ++h0;  // h0 point to first non-space character
	if (isalnum(*h0)){
		e0 = h0+1;
		while (!isspace(*e0) && e0!=str_end) ++e0;  // e0 point to the first word's end
		*e0 = '\0';
	}
	else {  // only a '\n' was given
		h0 = message;
		e0 = str_end;
	}

	int ret = 0, cmd = CMD_ERR;
	const char *command[] = {"ls", "quit", "put", "get", "play"};
	for (int i=0; i<5; ++i){
		ret = strcmp(h0, command[i]);
		if (ret == 0) {
			cmd = i;
			break;
		}
	}

	if (cmd > 1){  // put or get or play command
		h1 = e0+1;
		while (isspace(*h1)) ++h1;
		if (isalnum(*h1)){
			e1 = h1+1;
			while (!isspace(*e1) && e1!=str_end) ++e1;
			*e1 = '\0';
			strcpy(clt.filename, h1);
		}
		else {
			cmd = CMD_FMT;
		}
	}
	else if (cmd >= 0){
		h1 = e0+1;
		while (isspace(*h1)) ++h1;
		if (*h1 != '\0') cmd = CMD_FMT;
	}
	return cmd;
}


static void send_command(int cmd){
	clt.buf[0] = cmd & 0xff;
	strcpy(&clt.buf[1], clt.filename);
	clt.buf_len = strlen(clt.filename)+1;
	handle_write();
}

static void send_file(int fd){
	int file_size = getFilesize(fd);
	char print_buf[50];
	// tell server how many bytes would send
	clt.buf[0] = file_size & 0xff;
	clt.buf[1] = (file_size >> 8) & 0xff;
	clt.buf[2] = (file_size >> 16) & 0xff;
	clt.buf[3] = (file_size >> 24) & 0xff;
	clt.buf_len = 4;
	handle_write();
	fprintf(stdout, "Sending file \"%s\" ", clt.filename);
	sprintf(print_buf, "(0/%d bytes)", file_size);
	fprintf(stdout, "%s", print_buf);

	// transfer file content
	int read_bytes, sent_bytes=0;
	while ((read_bytes = read(fd, clt.buf, BUFF_SIZE)) > 0){
		clt.buf_len = read_bytes;
		handle_write();
		sent_bytes += read_bytes;
		fflush(stdout);
		for (int i=0; i<strlen(print_buf); ++i) fprintf(stdout, "\b");
		sprintf(print_buf, "(%d/%d bytes)", sent_bytes, file_size);
		fprintf(stdout, "%s", print_buf);
	}
	fprintf(stdout, "\n");
}

int getFilesize(int fd){
	off_t currentPos = lseek(fd, 0, SEEK_CUR);
	off_t file_size = lseek(fd, 0, SEEK_END);
	lseek(fd, currentPos, SEEK_SET);
	return (int)file_size;
}