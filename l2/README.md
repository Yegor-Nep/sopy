## 0. Boilerplate (`ERR`, `bulk_read`, `bulk_write`)

```c
#define _GNU_SOURCE
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
        count -= c;
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
        count -= c;
    } while (count > 0);
    return len;
}
```

---

## 1. Operacje niskopoziomowe (`open`, `lseek`, `-at`)

### Pułapki i kluczowe zasady
* `O_CREAT` **wymaga** 3. argumentu `mode_t` (np. `0644`).
* `O_CREAT | O_EXCL` – atomowe utworzenie pliku; błąd `EEXIST`, jeśli plik istnieje.
* `close(fd)` – w Linuksie **nigdy** nie powtarzaj w pętli na `EINTR` (deskryptor jest już zwolniony).
* `lseek` na FIFO/pipe/socket zwraca `-1` i `errno == ESPIPE`.
* Funkcje `-at` (`openat`, `fstatat`, `unlinkat`) przyjmują `dirfd` lub `AT_FDCWD` (bieżący katalog).

### Szablony

```c
// Otwarcie / atomowe utworzenie pliku
int fd = open("plik.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
if (fd < 0)
    ERR("open");

// Rozmiar pliku i powrót na początek przez lseek
off_t size = lseek(fd, 0, SEEK_END);
if (size == (off_t)-1)
    ERR("lseek");
if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
    ERR("lseek");

// Aktualna pozycja kursora
off_t cur = lseek(fd, 0, SEEK_CUR);
if (cur == (off_t)-1)
    ERR("lseek");

// Operacje względem deskryptora katalogu (-at)
int dir_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
if (dir_fd < 0)
    ERR("open dir");

struct stat st;
if (fstatat(dir_fd, "plik.bin", &st, 0) < 0)
    ERR("fstatat");

int sub_fd = openat(dir_fd, "plik.bin", O_RDONLY);
if (sub_fd < 0)
    ERR("openat");

close(sub_fd);
close(dir_fd);
close(fd);
```

---

## 2. Operacje wektorowe (`readv`, `writev`)

### Pułapki i kluczowe zasady
* `<sys/uio.h>` – odczyt/zapis wielu nieciągłych buforów w jednym wywołaniu systemowym (unikasz `memcpy`, zapis jest atomowy względem innych pisarzy).
* Tak jak `read`/`write`, mogą zostać przerwane (`EINTR`) lub wykonać transfer częściowy.

### Szablony

```c
uint32_t header = 123;
char payload[64] = "dane";

struct iovec iov[2] = {
    {.iov_base = &header, .iov_len = sizeof(header)},
    {.iov_base = payload, .iov_len = sizeof(payload)}
};

// Zapis wektorowy
ssize_t nw;
while ((nw = writev(fd, iov, 2)) < 0)
{
    if (errno == EINTR)
        continue;
    ERR("writev");
}

// Odczyt wektorowy
ssize_t nr;
while ((nr = readv(fd, iov, 2)) < 0)
{
    if (errno == EINTR)
        continue;
    ERR("readv");
}
```

---

## 3. Mapowanie plików w pamięć (`mmap`, `munmap`, `msync`)

### Pułapki i kluczowe zasady
* `length == 0` (pusty plik) → `EINVAL`.
* `offset` musi być wielokrotnością `sysconf(_SC_PAGE_SIZE)`.
* `PROT_WRITE | MAP_SHARED` wymaga `fd` otwartego z **`O_RDWR`** (samo `O_WRONLY` → `EACCES`).
* Błąd zwraca **`MAP_FAILED`** (`(void *)-1`), a **nie `NULL`**.
* `close(fd)` można wywołać od razu po `mmap()` – mapowanie pozostaje aktywne do `munmap()`.
* Wyjście poza rozmiar pliku → `SIGBUS`; zapis przy `PROT_READ` → `SIGSEGV`.

### Szablony

```c
// Mapowanie całego istniejącego pliku (R/W)
int fd = open("dane.bin", O_RDWR);
if (fd < 0)
    ERR("open");

struct stat st;
if (fstat(fd, &st) < 0)
    ERR("fstat");
if (st.st_size == 0)
    ERR("empty file");

size_t len = (size_t)st.st_size;
char *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
if (map == MAP_FAILED)
    ERR("mmap");

close(fd); // bezpieczne od razu po mmap

// Wymuszenie zrzutu zmian na dysk (MS_SYNC lub MS_ASYNC)
if (msync(map, len, MS_SYNC) < 0)
    ERR("msync");

if (munmap(map, len) < 0)
    ERR("munmap");
```

```c
// Mapowanie fragmentu pliku z wyrównaniem offsetu do rozmiaru strony
long page_size = sysconf(_SC_PAGE_SIZE);
off_t raw_offset = 5000;
off_t aligned_offset = (raw_offset / page_size) * page_size;
size_t delta = (size_t)(raw_offset - aligned_offset);
size_t map_len = delta + 256; // ile bajtów potrzebujesz od raw_offset

char *base = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, aligned_offset);
if (base == MAP_FAILED)
    ERR("mmap");

char *target = base + delta; // właściwy wskaźnik pod raw_offset
// ...
if (munmap(base, map_len) < 0)
    ERR("munmap");
```

---

## 4. Tryb nieblokujący i `fcntl`

### Pułapki i kluczowe zasady
* Gdy brak danych (lub miejsca w buforze zapisu), `read`/`write` z `O_NONBLOCK` zwraca `-1` i `errno` równe **`EAGAIN` lub `EWOULDBLOCK`** (zawsze sprawdzaj oba).
* Przy `F_SETFL` zawsze najpierw pobierz flagi przez `F_GETFL`, aby nie nadpisać trybu dostępu (`O_RDONLY` itp.).

### Szablony

```c
int set_nonblock(int fd, int enable)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return -1;
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}
```

```c
// Odczyt z deskryptora O_NONBLOCK
char buf[256];
ssize_t n = read(fd, buf, sizeof(buf));
if (n < 0)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    {
        // brak danych teraz - spróbuj później
    }
    else
        ERR("read");
}
else if (n == 0)
{
    // EOF
}
else
{
    // przetwórz buf[0..n-1]
}
```

---

## 5. Multipleksowanie I/O (`poll`)

### Pułapki i kluczowe zasady
* `fds[i].fd < 0` → jądro ignoruje ten slot i zeruje jego `revents` (idealne po zamknięciu pojedynczego deskryptora bez przesuwania tablicy).
* **FIFO na Linuksie**: gdy pisarz zamknie FIFO, a w buforze są jeszcze dane, `poll` ustawia jednocześnie `POLLIN | POLLHUP`. Czytaj aż `read()` zwróci `0`!
* Flagi `POLLHUP`, `POLLERR`, `POLLNVAL` pojawiają się w `revents` automatycznie (nie wpisuje się ich do `events`).

### Szablon

```c
// fds[0..nfds-1] zainicjalizowane: fds[i].fd = ..., fds[i].events = POLLIN
int active = (int)nfds;
char buf[256];

while (active > 0)
{
    int ready = poll(fds, nfds, -1); // -1 = bez timeoutu, 0 = non-blocking, >0 = ms
    if (ready < 0)
    {
        if (errno == EINTR)
            continue;
        ERR("poll");
    }

    for (nfds_t i = 0; i < nfds; i++)
    {
        if (fds[i].fd < 0 || fds[i].revents == 0)
            continue;

        if (fds[i].revents & (POLLIN | POLLHUP))
        {
            ssize_t n = read(fds[i].fd, buf, sizeof(buf));
            if (n < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                ERR("read");
            }
            else if (n == 0)
            {
                // EOF - wszyscy pisarze zamknęli deskryptor i bufor pusty
                close(fds[i].fd);
                fds[i].fd = -1;
                active--;
            }
            else
            {
                // przetwórz n bajtów z fds[i].fd
            }
        }
        else if (fds[i].revents & (POLLERR | POLLNVAL))
        {
            close(fds[i].fd);
            fds[i].fd = -1;
            active--;
        }
    }
}
```

---

## 6. Łącza nazwane (FIFO)

### Pułapki i kluczowe zasady
* `open(fifo, O_RDONLY)` (bez `O_NONBLOCK`) blokuje do pojawienia się pisarza; `open(fifo, O_WRONLY)` blokuje do pojawienia się czytelnika.
* `open(fifo, O_RDONLY | O_NONBLOCK)` wraca od razu; `open(fifo, O_WRONLY | O_NONBLOCK)` zwraca `-1` (`errno == ENXIO`), jeśli brak czytelnika.
* **Nie otwieraj FIFO z `O_RDWR`**, jeśli oczekujesz `EOF` / `POLLHUP` po wyjściu pisarzy (proces trzyma wtedy otwarty własny koniec piszący i nigdy nie dostanie `EOF`).
* Zapis przy braku czytelników generuje **`SIGPIPE`** (zabija proces); przy `signal(SIGPIPE, SIG_IGN)` `write` zwraca `-1` i `errno == EPIPE`.
* Zapisy $\le$ `PIPE_BUF` (4096 B w Linuksie) są **atomowe**.

### Szablony

```c
const char *fifo = "/tmp/lab2_fifo";

if (mkfifo(fifo, 0666) < 0 && errno != EEXIST)
    ERR("mkfifo");

// --- CZYTELNIK (nieblokujące open, żeby nie wisieć przed startem pisarza) ---
int rfd = open(fifo, O_RDONLY | O_NONBLOCK);
if (rfd < 0)
    ERR("open fifo read");
// Opcjonalnie wyłącz O_NONBLOCK po otwarciu: set_nonblock(rfd, 0);

// --- PISARZ ---
signal(SIGPIPE, SIG_IGN); // zamiast zabicia procesu dostaniesz EPIPE
int wfd = open(fifo, O_WRONLY); // czeka aż pojawi się czytelnik
if (wfd < 0)
    ERR("open fifo write");

const char msg[] = "hello\n";
if (bulk_write(wfd, msg, sizeof(msg) - 1) < 0)
{
    if (errno == EPIPE)
        fprintf(stderr, "Brak czytelnikow!\n");
    else
        ERR("bulk_write");
}

close(wfd);
close(rfd);
unlink(fifo);
```