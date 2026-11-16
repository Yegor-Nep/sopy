#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>
#include <inttypes.h>
#include <sys/timerfd.h>
#include <sys/mman.h>

#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

void usage(const char *const pname)
{
    fprintf(stderr, "USAGE:%s path_1 path_2\n", pname);
    exit(EXIT_FAILURE);
}

#define LINES 32
#define SIZE 64
#define MAX_MESSAGE 128

char* join_paths(const char* path1, const char* path2) {
    char* res;
    const int l1 = strlen(path1);
    if (path1[l1 - 1] == '/') {
        res = malloc(strlen(path1) + strlen(path2) + 1);
        if (!res) ERR("malloc");
        strcpy(res, path1);
    } else {
        res = malloc(strlen(path1) + strlen(path2) + 2);  
        if (!res) ERR("malloc");
        strcpy(res, path1);
        res[l1] = '/';
        res[l1 + 1] = '\0';
    }
    return strcat(res, path2);
}

void check_recipe(const char *filename) {
    int fd = open(filename, O_CREAT|O_RDWR|O_EXCL, 0777);

    if(fd == -1 && errno != EEXIST) {
        ERR("open");
    }

    if(errno == EEXIST) {
        fd = open(filename, O_RDWR);
        off_t len = 0;

        len = lseek(fd, 0, SEEK_END);

        if(len == -1) {
            ERR("lseek");
        }

        if(len != 2048) {
            fprintf(stderr, "Ta księga to nie przepis!\n");
            exit(0);
        }
    }
    else {
        char buf[SIZE];
        for(int i = 0; i < SIZE-1; i++) {
            buf[i] = ' ';
        }
        buf[SIZE-1] = '\n';

        for(int i = 0; i < LINES; i++) {
            if(write(fd, buf, SIZE) == -1) {
                ERR("write");
            }

        }
    }

    if(close(fd) == -1)
        ERR("close");
}

void replace_line(char *line, const char *filename) {
    char numbers[3], text[64];

    char *pos = strchr(line, ':');
    if(pos == NULL || pos-line != 2) {
        fprintf(stderr,"Babcia sie zagadala\n");
        return;
    }
    
    strncpy(numbers, line, 2);
    numbers[2] = '\0';
    int nums = atoi(numbers);
    if((nums <= 0 && numbers[0] != '0' && numbers[1] != '0') || nums > 31) {
        fprintf(stderr,"Babcia sie zagadala\n");
        return;
    }

    int size = strlen(line)-4;
    if(size > 63) {
        size = 63;
    }
    strncpy(text, line+3, size);
    for(int i = size; i < 63; i++) {
        text[i] = ' ';
    }
    int fd = open(filename, O_RDWR);
    if(fd == -1) ERR("open");

    text[63] = '\0';
    lseek(fd, SIZE*nums, SEEK_SET);
    write(fd, text, strlen(text));

    if(close(fd == -1)) ERR("close");

}

void read_tower(const char *path, const char *filename) {
    int fd = open(path, O_RDONLY);
    if(fd == -1 && errno == ENOENT) {
        fprintf(stderr, "Nie ma babci na wiezy %s\n", path);
        exit(0);
    }
    else if(fd == -1) ERR("open");

    struct stat statbuf;

    if(stat(path, &statbuf) == -1) ERR("stat");

    if(!S_ISFIFO(statbuf.st_mode)) {
        fprintf(stderr, "Nie ma babci na wiezy %s\n", path);
        exit(0);
    }
    
    ssize_t bytes_read;
    char buf[4096];
    size_t taken = 0;
    char line[MAX_MESSAGE+1];

    while((bytes_read = read(fd, buf+taken, sizeof(buf)-taken)) > 0) {
        taken += bytes_read;
        char *sptr;

        while ((sptr = memchr(buf, '\n', taken)) != NULL) {
            strncpy(line, buf, sptr-buf+1);

            line[sptr-buf+1] = '\0';

            replace_line(line, filename);

            taken -= sptr-buf+1;
            memmove(buf, sptr+1, taken);
        }
    }
    if(bytes_read == -1) ERR("read");

    close(fd);
}

int main(int argc, char **argv) {
    if(argc < 3 || argc > 6) {
        usage(argv[0]);
    }    

    check_recipe(argv[1]);
    // read_tower(argv[2], argv[1]);


    int granny_count = argc-2;
    int n = argc-2;
    struct pollfd fds[n];

    char buff[n][LINES*MAX_MESSAGE];
    int taken[n];


    for(int i = 0; i < n; i++) {
        fds[i].fd = open(argv[i+2], O_RDONLY);
        if(fds[i].fd == -1) ERR("open");
        fds[i].events = POLLIN;
        fds[i].revents = 0;
        taken[i] = 0;
    }

    while(granny_count > 0) {
        int ret = poll(fds, n, 3000);
        if(ret < 0) ERR("poll");
        if(ret == 0) {
            printf("Babcie dolewaja herbatki\n");
            continue;
        }

        for(int i = 0; i < n; i++) {
            struct stat buf;
            if (fstat(fds[i].fd, &buf) == -1) { 
                continue;
            }

            if(fds[i].fd < 0)
                continue;
            if (fds[i].revents & (POLLERR | POLLNVAL))
            {
                close(fds[i].fd);
                granny_count--;
                printf("Babcia z wiezy %s skonczyla gadać\n", argv[i+2]);
                fds[i].fd = -1;
                continue;
            }

            if (!(fds[i].revents & (POLLIN|POLLHUP)))
                continue;

            char line[MAX_MESSAGE+1];
            ssize_t r;

            r = read(fds[i].fd, buff[i]+taken[i], sizeof(buff[i])-taken[i]);
            if(r < 0) ERR("read");
            if(r == 0) {
                printf("Babcia z wiezy %s skonczyla gadać\n", argv[i+2]);
                if(close(fds[i].fd) == -1) ERR("close");
                granny_count--;
                fds[i].fd = -1;
                continue;
            }
            taken[i] += r;
            char *sptr;

            while ((sptr = memchr(buff[i], '\n', taken[i])) != NULL) {
                strncpy(line, buff[i], sptr-buff[i]+1);

                line[sptr-buff[i]+1] = '\0';

                replace_line(line, argv[1]);

                taken[i] -= sptr-buff[i]+1;
                memmove(buff[i], sptr+1, taken[i]);
            }
        }
    }

    int fd = open(argv[1], O_RDWR);
    if(fd == -1) ERR("open");
    struct stat sb;
    if(fstat(fd, &sb) == -1) ERR("fstat");
    char *buffer = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(buff == MAP_FAILED) ERR("mmap");

    int N = 0;
    char *pos = buffer;    
    while((pos = memmem(buffer+(pos-buffer),sb.st_size - (pos  - buffer),"raw~berries", 11)) != NULL) {
        pos[2] = 's';
        pos[3] = 'p';
        N++;
    }
    msync(buffer, sb.st_size, MS_SYNC);
    int K = 0;
    int t = 1;
    int lines = sb.st_size / SIZE;

    for (int i = 0; i < lines; i++) {
        char *line = buffer + i * SIZE;

        int len = 63;
        while (len > 0 && line[len - 1] == ' ') {
            len--;
        }

        if (len == 0) {
            K++;
        } else {
            printf("%02d: %.*s\n", i, len, line);
        }
    }
    munmap(buffer, sb.st_size);

    return EXIT_SUCCESS;
}