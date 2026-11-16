#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <bits/types/struct_iovec.h>
#include <stdint.h>
#include <sys/poll.h>
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/uio.h>

#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

#define MAX_LEN 128

void usage(const char *const pname) {
    fprintf(stderr, "USAGE: %s katalog_roboczy liczba_kanalow_N interwal_ms\n", pname);
    exit(EXIT_FAILURE);
}

void set_nonblock(int fd) { 
    int flags = fcntl(fd, F_GETFL);
    if(flags == -1) ERR("fcntl");
    flags |= O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flags) == -1) ERR("fcntl");
}

struct bin {
    uint32_t num;
    uint32_t len;
    uint64_t seq_num;
};

struct binary {
    uint64_t msg_count;
    uint64_t bytes_total;
    uint64_t last_seq;
};

void print_stats(struct binary *buff,  int N) {
    for(int i = 0; i < N; i++) {
        printf("Channel %d: msgs=%ld, bytes=%ld , last_seq=%ld PRIu64 \n", i, buff[i].msg_count, buff[i].bytes_total, buff[i].last_seq);
    }
}

int main(int argc, char **argv) {
    if(argc != 4) usage(argv[0]);
    int N = atoi(argv[2]);
    if(N <= 0) usage(argv[0]);
    off_t size = N*8;

    int dir_fd = open(argv[1], O_RDONLY|O_DIRECTORY);
    if(dir_fd == -1) ERR("open");

    int fd_bin = openat(dir_fd, "stats.bin", O_RDWR|O_CREAT|O_TRUNC, 0640);
    if(fd_bin == -1) ERR("openat");

    if(ftruncate(fd_bin, size)==-1) ERR("ftruncate");
    struct stat sb; 
    if(fstat(fd_bin, &sb) == -1) ERR("fstat");

    struct binary *buff = mmap(NULL, sb.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd_bin, 0);
    if(buff == MAP_FAILED) ERR("mmap");


    int fd_log = openat(dir_fd, "events.log", O_RDWR|O_CREAT|O_TRUNC, 0640);
    if(fd_log == -1) ERR("openat");
    if(write(fd_log, "SOPLOG01\n", 9) == -1) ERR("write");

    fprintf(stderr, "%ld\n", lseek(fd_log, 0, SEEK_CUR));

    set_nonblock(STDIN_FILENO);

    char name[6] = "in_NN"; 
    struct pollfd fds[N+1];

    for(int i = 0; i < N; i++) {
        if(i < 10) {
            name[3] = i + '0';
            name[4] = '\0';
        }
        else {
            name[3] = i/10 + '0';
            name[4] = i%10 + '0';
            name[5] = '\0';
        }

        if(unlinkat(dir_fd, name, 0) == -1 && errno != ENOENT) ERR("unlinkat");
    
        if(mkfifoat(dir_fd, name, 0660) == -1) ERR("mkfifoat");

        fds[i].fd = openat(dir_fd, name, O_RDONLY | O_NONBLOCK);
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    fds[N].fd = STDIN_FILENO;
    fds[N].events = POLLIN;
    fds[N].revents = 0;

    uint64_t seq = 1;
    int running = 1;

    while(running) {
        int ret = poll(fds, N+1, -1);
        if(ret < 0) ERR("poll");

        for(int i = 0; i <= N; i++) {
            if(fds[i].fd < 0) continue;

            if ((fds[i].revents & POLLIN)) {

                ssize_t r;
                char buf[MAX_LEN+1];
                int taken = 0;
                while(taken < MAX_LEN && (r = read(fds[i].fd, buf + taken, MAX_LEN - taken)) > 0) {
                    taken += r;
                }
                if(r < 0) {
                    if(errno != EAGAIN && errno != EWOULDBLOCK)
                        ERR("read");
                }
                if(r == 0) {
                    if(i == N) { 
                        running = 0;
                    }
                    else {
                        if(close(fds[i].fd) == -1) ERR("close");
                        fds[i].fd = -1;
                    }
                }
                if(taken > 0) {
                    buf[taken] = '\0';
                    if(i < N) {
                        char *ptr;
                        while((ptr = strchr(buf, '\n')) != NULL) {
                            char line[MAX_LEN+1]; 
                            memcpy(line, buf, ptr - buf);
                            line[ptr-buf] = '\0';
                            taken -= ptr-buf+1;
                            memmove(buf, buf+(ptr-buf+1), taken+1);
                            uint32_t len = strlen(line);
                            struct iovec iov[3];
                            struct bin new_bin; 
                            char c = '\n';
                            new_bin.num = i;
                            new_bin.len = len;
                            new_bin.seq_num = seq;
                            iov[0].iov_base = &new_bin;
                            iov[0].iov_len = sizeof(new_bin);
                            iov[1].iov_len = len; 
                            iov[1].iov_base = line;
                            iov[2].iov_len = 1;
                            iov[2].iov_base = &c; 

                            int indx = 0;
                            ssize_t bytes;
                            int iovcnt = sizeof(iov)/sizeof(struct iovec);
                            while(indx < iovcnt && (bytes = writev(fd_log, iov + indx, iovcnt - indx)) > 0) {
                                while(indx < iovcnt && bytes >= (ssize_t)iov[indx].iov_len) {
                                    bytes -= iov[indx].iov_len;
                                    indx++;
                                }
                                if(indx < iovcnt && bytes > 0) {
                                    iov[indx].iov_base += bytes;
                                    iov[indx].iov_len -= bytes;
                                }
                            }
                            if(bytes < 0) ERR("writev");
                            buff[i].msg_count++;
                            buff[i].bytes_total += len;
                            buff[i].last_seq = seq;
                            seq++;
                        }
                    }
                    else {
                        buf[taken-1] = '\0';
                        if(strcmp(buf, "QUIT") == 0) {
                            running = 0;
                        }
                        if(strcmp(buf, "STAT") == 0) {
                            print_stats(buff, N);
                        }
                        if(strstr(buf, "RESET") != NULL) {
                            int buflen = strlen(buf); 
                            if(buflen <= 8 && buflen >= 7) {
                                char stringNum[3];
                                stringNum[0] = buf[6];
                                if(buflen == 7) {
                                    stringNum[1] = '\0';
                                }
                                else {
                                    stringNum[1] = buf[7];
                                    stringNum[2] = '\0';
                                }

                                int number = atoi(stringNum);
                                if(number == 0 && stringNum[0] != '0') {
                                    continue;
                                }
                                if(number <= N-1) {
                                    buff[number].bytes_total = 0;
                                    buff[number].last_seq = 0;
                                    buff[number].msg_count = 0;
                                    if(msync(buff, sb.st_size, MS_SYNC) == -1) ERR("msync");
                                }
                            }
                        }
                    }
                }
            }
                


            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                if(fds[i].fd >= 0) {
                    if(close(fds[i].fd) == -1) ERR("close");
                    fds[i].fd = -1;
                }
                continue;
            }
        }
    }

    if(msync(buff, sb.st_size, MS_SYNC) == -1) ERR("msync");
    if(munmap(buff, sb.st_size) == -1) ERR("munmap");
    for(int i = 0; i < N; i++) {
        char namein[5];
        sprintf(namein, "in_%d", i);
        if(unlinkat(dir_fd, namein, 0) != 0 && errno != ENOENT) ERR("unlinkat");
    }
    if(close(fd_bin) == -1) ERR("close");
    if(close(fd_log) == -1) ERR("close");
    if(close(dir_fd) == -1) ERR("close");
    

    return EXIT_SUCCESS;
}