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
    fprintf(stderr, "USAGE: %s plik_bazy plik_skryptu\n", pname);
    exit(EXIT_FAILURE);
}

typedef struct {
    char *key;
    char *val;
} Entry;

char *delete_invisible_characters_right(char *ptr, const char *limit) {
    while(ptr >= limit && (*ptr == ' ' || *ptr == '\t' || *ptr == '\r')) {
        ptr--;
    }
    return ptr;
}
char *delete_invisible_characters_left(char *ptr, const char *limit) {
    while(ptr <= limit && (*ptr == ' ' || *ptr == '\t' || *ptr == '\r')) {
        ptr++;
    
    }
    return ptr;
}

void add_entry(Entry **buff, int *count, int *capacity, char *key, char *val) {
    if(*capacity == 0) {
        *capacity = 8;
        *buff = realloc(NULL, *capacity * sizeof(Entry));
    }

    if(*count == *capacity) {
        *capacity *= 2;
        *buff = realloc(*buff, (*capacity) * sizeof(Entry));
        if(!*buff) ERR("realloc");
    }


    (*buff)[*count].key = key;
    (*buff)[*count].val = val;
    (*count)++;
}

int main(int argc, char **argv) {
    if(argc != 3) usage(argv[0]);

    int fd_base = open(argv[1], O_CREAT|O_RDWR, 0644);
    if(fd_base == -1) ERR("open");

    struct stat sb;
    if(fstat(fd_base, &sb) == -1) ERR("fstat");
    Entry *buff = NULL;
    int count = 0;
    int capacity = 0;
    char *map; 
    if(sb.st_size != 0) {
        map = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd_base, 0);
        if(map == MAP_FAILED) ERR("mapp");

        char *cur = map;
        const char *end = map + sb.st_size;
        char *posEnd;
        char *nextLine;
        while(cur < end) {
            posEnd = memchr(cur, '\n', end-cur);
            if(posEnd == NULL) {
                posEnd = (char *)end;
                nextLine = posEnd;
            }
            else {
                nextLine = posEnd + 1;
            }
            if(*cur == '#') {
                cur = nextLine;
                continue;
            }
            char *posEqual;
            posEqual = memchr(cur, '=', posEnd-cur);
            if(posEqual == NULL) {
                cur = nextLine;
                continue;
            }            
            char *valueBegin = posEqual;
            posEnd--;
            posEnd = delete_invisible_characters_right(posEnd, posEqual+1);
            posEqual--;
            posEqual = delete_invisible_characters_right(posEqual, cur);
            cur = delete_invisible_characters_left(cur, posEqual);
            valueBegin++;
            valueBegin = delete_invisible_characters_left(valueBegin, posEnd);
            
            char *key, *value;
            key = strndup(cur, posEqual-cur+1);
            value = strndup(valueBegin, posEnd-valueBegin+1);
            if(!key || !value) ERR("strndup");

            add_entry(&buff, &count, &capacity, key, value);
            cur = nextLine;

        }
        munmap(map, sb.st_size);
    }


    if(close(fd_base) == -1) ERR("close");
    return EXIT_SUCCESS;
}