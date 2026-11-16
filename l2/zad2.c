#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))
#define SIZE 31
#define RECORD_SIZE (SIZE + 1)

void usage(const char *const pname) {
    fprintf(stderr, "USAGE: %s raport_path stacja1 [stacja2 ... stacja6]\n", pname);
    exit(EXIT_FAILURE);
}

void write_data(const char *line, const char *report_path, int max_records) {
    char *pos = strchr(line, ':');
    if (pos == NULL || pos - line != 2 || !isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1])) {
        fprintf(stderr, "Zaklocenia na linii: %s", line);
        return;
    }

    int num = (line[0] - '0') * 10 + (line[1] - '0');
    if (num < 0 || num >= max_records) {
        fprintf(stderr, "Zaklocenia na linii: %s", line);
        return;
    }

    int space = (int)strlen(line) - 4;
    if (space < 1 || space > 28) {
        fprintf(stderr, "Zaklocenia na linii: %s", line);
        return;
    }

    char text[SIZE];
    memcpy(text, line + 3, space);
    for (int i = space; i < SIZE; i++) {
        text[i] = ' ';
    }

    int fd = open(report_path, O_RDWR);
    if (fd == -1) ERR("open");

    if (lseek(fd, (off_t)num * RECORD_SIZE, SEEK_SET) == -1) ERR("lseek");
    if (write(fd, text, SIZE) != SIZE) ERR("write");
    if (close(fd) == -1) ERR("close");

    printf("Zaktualizowano region %02d\n", num);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 8) {
        usage(argv[0]);
    }

    struct stat sb;
    if (stat(argv[1], &sb) == -1) {
        if (errno == ENOENT) {
            fprintf(stderr, "Plik nie istnieje\n");
            exit(EXIT_FAILURE);
        }
        ERR("stat");
    }
    if (!S_ISREG(sb.st_mode)) {
        fprintf(stderr, "Plik nie jest regularny\n");
        exit(EXIT_FAILURE);
    }

    int max_records = sb.st_size / RECORD_SIZE;

    int n = argc - 2;
    int count = n;
    struct pollfd fds[n];
    char buffer[n][4096];
    int taken[n];

    for (int i = 0; i < n; i++) {
        taken[i] = 0;
        struct stat statb;
        if (stat(argv[i + 2], &statb) == -1) {
            fprintf(stderr, "Brak polaczenia ze stacja %s\n", argv[i + 2]);
            exit(EXIT_FAILURE);
        }
        if (!S_ISFIFO(statb.st_mode)) {
            fprintf(stderr, "Brak polaczenia ze stacja %s\n", argv[i + 2]);
            exit(EXIT_FAILURE);
        }

        fds[i].fd = open(argv[i + 2], O_RDONLY);
        if (fds[i].fd == -1) ERR("open fifo");
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }

    while (count > 0) {
        int ret = poll(fds, n, 2000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            ERR("poll");
        }
        if (ret == 0) {
            printf("Oczekiwanie na sygnal ze stacji...\n");
            continue;
        }

        for (int i = 0; i < n; i++) {
            if (fds[i].fd < 0) continue;

            if (fds[i].revents & (POLLERR | POLLNVAL)) {
                if (close(fds[i].fd) == -1) ERR("close");
                count--;
                fds[i].fd = -1;
                printf("Stacja %s zakonczyla nadawanie\n", argv[i + 2]);
                continue;
            }
            if (!(fds[i].revents & (POLLIN | POLLHUP))) {
                continue;
            }

            ssize_t r = read(fds[i].fd, buffer[i] + taken[i], sizeof(buffer[i]) - taken[i]);
            if (r < 0) ERR("read");
            if (r == 0) {
                if (close(fds[i].fd) == -1) ERR("close");
                count--;
                fds[i].fd = -1;
                printf("Stacja %s zakonczyla nadawanie\n", argv[i + 2]);
                continue;
            }

            taken[i] += r;

            char line[256];
            char *sptr;
            while ((sptr = memchr(buffer[i], '\n', taken[i])) != NULL) {
                size_t msg_len = sptr - buffer[i] + 1; // długość razem z '\n'
                if (msg_len >= sizeof(line)) {
                    fprintf(stderr, "Zaklocenia na linii (za dluga)\n");
                } else {
                    memcpy(line, buffer[i], msg_len);
                    line[msg_len] = '\0';
                    write_data(line, argv[1], max_records);
                }

                taken[i] -= msg_len;
                memmove(buffer[i], sptr + 1, taken[i]);
            }
        }
    }

    int fd = open(argv[1], O_RDWR);
    if (fd == -1) ERR("open");
    struct stat sbuff;
    if (fstat(fd, &sbuff) == -1) ERR("fstat");

    char *buff = mmap(NULL, sbuff.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buff == MAP_FAILED) ERR("mmap");

    char *pos = buff;
    int A = 0;
    int NoData = 0;

    while ((pos = memmem(pos, sbuff.st_size - (pos - buff), "ERR", 3)) != NULL) {
        A++;
        pos[0] = '-';
        pos[1] = '-';
        pos[2] = '-';
        pos += 3;
    }

    int total_records = sbuff.st_size / RECORD_SIZE;
    for (int i = 0; i < total_records; i++) {
        char *line = buff + i * RECORD_SIZE;
        int len = SIZE;

        if (strncmp(line, "BRAK DANYCH", 11) == 0) {
            NoData++;
            continue;
        }
        while (len > 0 && line[len - 1] == ' ') {
            len--;
        }
        if (len > 0) {
            printf("Region %02d -> %.*s\n", i, len, line);
        }
    }

    printf("Regiony bez danych: %d, wykryte awarie: %d\n", NoData, A);

    if (msync(buff, sbuff.st_size, MS_SYNC) == -1) ERR("msync");
    if (munmap(buff, sbuff.st_size) == -1) ERR("munmap");
    if (close(fd) == -1) ERR("close");

    return EXIT_SUCCESS;
}