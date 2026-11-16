## 0. Nagłówki i funkcje bazowe (`ERR`, `bulk_read`, `bulk_write`)

Podstawowy szkielet pliku z obsługą błędów oraz pełnym odczytem/zapisem odpornym na `EINTR` i częściowe transfery.

```c
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

ssize_t bulk_read(int fd, char *buf, size_t count)
{
    ssize_t c, len = 0;
    do
    {
        c = read(fd, buf, count);
        if (c < 0)
        {
            if (errno == EINTR)
                continue;
            return c;
        }
        if (c == 0)
            return len;
        buf += c;
        len += c;
        count -= (size_t)c;
    } while (count > 0);
    return len;
}

ssize_t bulk_write(int fd, const char *buf, size_t count)
{
    ssize_t c, len = 0;
    do
    {
        c = write(fd, buf, count);
        if (c < 0)
        {
            if (errno == EINTR)
                continue;
            return c;
        }
        buf += c;
        len += c;
        count -= (size_t)c;
    } while (count > 0);
    return len;
}
```

---

## 1. Dzielenie strumienia na linie (`StreamBuf` + `memchr` + `memmove`)

Uniwersalny bufor do czytania po linii z FIFO, potoku lub `STDIN` – radzi sobie z niepełnymi liniami, kilkoma liniami w jednym `read()` oraz zabezpiecza przed zablokowaniem przy przepełnieniu bufora.

```c
#define STREAM_BUF_SIZE 4096

typedef struct {
    char buf[STREAM_BUF_SIZE];
    size_t len;
} StreamBuf;

// Wyciąga jedną pełną linię (bez '\n') do `out`.
// Zwraca: 1 = pobrano linię, 0 = brak pełnej linii w buforze.
int stream_pop_line(StreamBuf *sb, char *out, size_t out_size)
{
    char *nl = memchr(sb->buf, '\n', sb->len);
    if (!nl)
    {
        // Zabezpieczenie przed przepełnieniem bufora linią bez '\n'
        if (sb->len == sizeof(sb->buf))
            sb->len = 0;
        return 0;
    }

    size_t line_len = (size_t)(nl - sb->buf);
    size_t copy_len = (line_len < out_size - 1) ? line_len : (out_size - 1);
    memcpy(out, sb->buf, copy_len);
    out[copy_len] = '\0';

    size_t consumed = line_len + 1; // +1 za '\n'
    sb->len -= consumed;
    memmove(sb->buf, nl + 1, sb->len);
    return 1;
}
```

---

## 2. Pętla `poll` dla wielu deskryptorów (FIFO + opcjonalnie `STDIN` i `timeout`)

Gotowy szkielet multipleksowania I/O połączony ze `StreamBuf` – automatycznie zamyka deskryptory po EOF/błędzie i wygasza je przez `fds[i].fd = -1`.

```c
// fds[0..nfds-1] zainicjalizowane (.fd = ..., .events = POLLIN)
int active = (int)nfds;
int running = 1;
StreamBuf sb[nfds];
memset(sb, 0, sizeof(sb));

while (running && active > 0)
{
    int ret = poll(fds, nfds, 2000); // -1 = bez limitu, >0 = timeout w ms
    if (ret < 0)
    {
        if (errno == EINTR)
            continue;
        ERR("poll");
    }
    if (ret == 0)
    {
        /* TODO: akcja przy timeout (jeśli timeout > 0) */
        continue;
    }

    for (nfds_t i = 0; i < nfds; i++)
    {
        if (fds[i].fd < 0 || fds[i].revents == 0)
            continue;

        if (fds[i].revents & (POLLERR | POLLNVAL))
        {
            if (close(fds[i].fd) == -1)
                ERR("close");
            fds[i].fd = -1;
            active--;
            continue;
        }

        if (fds[i].revents & (POLLIN | POLLHUP))
        {
            ssize_t r = read(fds[i].fd, sb[i].buf + sb[i].len, sizeof(sb[i].buf) - sb[i].len);
            if (r < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                ERR("read");
            }
            if (r == 0)
            {
                // EOF na deskryptorze i
                if (close(fds[i].fd) == -1)
                    ERR("close");
                fds[i].fd = -1;
                active--;
                /* TODO: opcjonalna reakcja na zamknięcie źródła i (np. running = 0 dla STDIN) */
                continue;
            }

            sb[i].len += (size_t)r;

            char line[512];
            while (stream_pop_line(&sb[i], line, sizeof(line)))
            {
                /* TODO: obsłuż pojedynczą linię `line` z deskryptora `i` */
            }
        }
    }
}
```

---

## 3. Tworzenie i otwieranie `N` kolejek FIFO w katalogu (`openat` + `mkfifoat`)

Wzorzec do utworzenia `N` nazwanych potoków w zadanym katalogu roboczym i otwarcia ich w trybie nieblokującym do odczytu[cite: 2].

```c
int dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
if (dir_fd == -1)
    ERR("open dir");

struct pollfd fds[N];
char fifo_name[32];

for (int i = 0; i < N; i++)
{
    snprintf(fifo_name, sizeof(fifo_name), "in_%d", i);

    if (unlinkat(dir_fd, fifo_name, 0) == -1 && errno != ENOENT)
        ERR("unlinkat");
    if (mkfifoat(dir_fd, fifo_name, 0660) == -1)
        ERR("mkfifoat");

    fds[i].fd = openat(dir_fd, fifo_name, O_RDONLY | O_NONBLOCK);
    if (fds[i].fd == -1)
        ERR("openat fifo");
    fds[i].events = POLLIN;
    fds[i].revents = 0;
}

// Sprzątanie na koniec programu:
// for (int i = 0; i < N; i++) {
//     snprintf(fifo_name, sizeof(fifo_name), "in_%d", i);
//     unlinkat(dir_fd, fifo_name, 0);
// }
// close(dir_fd);
```

---

## 4. Pełny zapis wektorowy (`bulk_writev`)

Zapis wielu nieciągłych fragmentów pamięci jednym wywołaniem systemowym, odporny na `EINTR` i częściowe zapisy[cite: 2].

```c
ssize_t bulk_writev(int fd, struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    int idx = 0;

    while (idx < iovcnt)
    {
        ssize_t nw = writev(fd, iov + idx, iovcnt - idx);
        if (nw < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (nw == 0)
            break;

        total += nw;
        while (idx < iovcnt && nw >= (ssize_t)iov[idx].iov_len)
        {
            nw -= (ssize_t)iov[idx].iov_len;
            idx++;
        }
        if (idx < iovcnt && nw > 0)
        {
            iov[idx].iov_base = (char *)iov[idx].iov_base + nw;
            iov[idx].iov_len -= (size_t)nw;
        }
    }
    return total;
}
```

---

## 5. Inicjalizacja pliku o stałym rozmiarze i mapowanie tablicy (`mmap`)

### A. Otwórz istniejący plik (z walidacją rozmiaru) lub utwórz i wypełnij nowy
```c
int open_or_init_file(const char *path, off_t expected_size, char fill_byte)
{
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd == -1)
    {
        if (errno != EEXIST)
            ERR("open");

        fd = open(path, O_RDWR);
        if (fd == -1)
            ERR("open existing");

        off_t size = lseek(fd, 0, SEEK_END);
        if (size == (off_t)-1)
            ERR("lseek");
        if (size != expected_size)
        {
            fprintf(stderr, "Nieprawidlowy rozmiar pliku\n");
            close(fd);
            exit(EXIT_FAILURE);
        }
        return fd;
    }

    char block[4096];
    memset(block, fill_byte, sizeof(block));
    off_t rem = expected_size;
    while (rem > 0)
    {
        size_t chunk = (rem > (off_t)sizeof(block)) ? sizeof(block) : (size_t)rem;
        if (bulk_write(fd, block, chunk) != (ssize_t)chunk)
            ERR("bulk_write");
        rem -= (off_t)chunk;
    }
    return fd;
}
```

### B. Utworzenie pliku binarnego o zadanym rozmiarze (`ftruncate`) i zmapowanie go przez `mmap`
```c
// Zwraca wskaźnik zmapowany przez mmap; zwalniany przez munmap(ptr, total_bytes)
void *create_and_mmap(int dir_fd, const char *filename, size_t total_bytes)
{
    int fd = openat(dir_fd, filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
        ERR("openat");

    if (ftruncate(fd, (off_t)total_bytes) == -1)
        ERR("ftruncate");

    void *ptr = mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED)
        ERR("mmap");

    if (close(fd) == -1)
        ERR("close");
    return ptr;
}
```

---

## 6. Nadpisanie rekordu o stałej długości w pliku (`lseek` + padding)

Ustawia kursor na `record_idx * stride` i zapisuje `data_len` bajtów dopełnionych znakiem `pad` (np. `stride = 64, data_len = 63, pad = ' '`, gdy ostatni bajt rekordu to `\n`).

```c
void write_fixed_record(int fd, size_t record_idx, size_t stride, size_t data_len, const char *text, char pad)
{
    char buf[data_len];
    size_t len = strlen(text);
    if (len > data_len)
        len = data_len;

    memset(buf, pad, data_len);
    memcpy(buf, text, len);

    if (lseek(fd, (off_t)(record_idx * stride), SEEK_SET) == (off_t)-1)
        ERR("lseek");
    if (bulk_write(fd, buf, data_len) != (ssize_t)data_len)
        ERR("bulk_write");
}
```

---

## 7. Przetwarzanie tekstu i podmiana wzorca w `mmap` (`memchr`, `memmem`, `trim`)

### A. Przycinanie białych znaków i iteracja po liniach w buforze `mmap`
```c
const char *trim_left(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p))
        p++;
    return p;
}

const char *trim_right(const char *start, const char *p)
{
    while (p > start && isspace((unsigned char)p[-1]))
        p--;
    return p;
}

void scan_mmap_lines(const char *map, size_t size)
{
    const char *cur = map;
    const char *end = map + size;

    while (cur < end)
    {
        const char *nl = memchr(cur, '\n', (size_t)(end - cur));
        const char *line_end = nl ? nl : end;

        const char *s = trim_left(cur, line_end);
        const char *e = trim_right(s, line_end);

        if (s < e) // niepusta linia [s, e)
        {
            /* TODO: przetwórz fragment o długości (size_t)(e - s) */
        }

        cur = nl ? nl + 1 : end;
    }
}
```

### B. Podmiana wszystkich wystąpień wzorca tej samej długości w `mmap` (`memmem`)
```c
int mmap_replace_all(char *map, size_t size, const char *pattern, const char *replacement, size_t len)
{
    int count = 0;
    char *pos = map;
    char *end = map + size;

    while (pos < end && (pos = memmem(pos, (size_t)(end - pos), pattern, len)) != NULL)
    {
        memcpy(pos, replacement, len);
        pos += len;
        count++;
    }
    return count;
}
```

---

## 8. Pomocnicze: dynamiczna tablica (`realloc`), łączenie ścieżek, typ pliku

```c
// 1. Rozszerzanie dynamicznej tablicy dowolnego typu T (count, cap zainicjalizowane na 0)
#define DYN_PUSH(arr, count, cap, item)                                      \
    do                                                                       \
    {                                                                        \
        if ((count) >= (cap))                                                \
        {                                                                    \
            size_t new_cap = ((cap) == 0) ? 8 : (cap) * 2;                   \
            void *tmp = realloc((arr), new_cap * sizeof(*(arr)));            \
            if (!tmp)                                                        \
                ERR("realloc");                                              \
            (arr) = tmp;                                                     \
            (cap) = new_cap;                                                 \
        }                                                                    \
        (arr)[(count)++] = (item);                                           \
    } while (0)

// 2. Bezpieczne łączenie katalogu i nazwy pliku (wynik należy zwolnić przez free())
char *join_paths(const char *dir, const char *file)
{
    size_t l1 = strlen(dir);
    size_t l2 = strlen(file);
    int need_slash = (l1 > 0 && dir[l1 - 1] != '/');

    char *res = malloc(l1 + (size_t)need_slash + l2 + 1);
    if (!res)
        ERR("malloc");

    memcpy(res, dir, l1);
    if (need_slash)
        res[l1] = '/';
    memcpy(res + l1 + (size_t)need_slash, file, l2 + 1);
    return res;
}

// 3. Sprawdzenie typu pliku (np. S_IFREG dla pliku zwykłego, S_IFIFO dla FIFO)
int has_file_mode(const char *path, mode_t file_type)
{
    struct stat sb;
    if (stat(path, &sb) == -1)
        return 0;
    return (sb.st_mode & S_IFMT) == file_type;
}
```