## 1. Wyciąganie linii i tokenów

```c
// Wyciąga fragment do znaku `delim` (np. '\n').
// Jeśli nie ma `delim`, zwraca NULL i nie rusza wskaźników (czeka na kolejne read()).
char *next_chunk_strict(char **buf, size_t *len, char delim, size_t *tok_len) {
    if (!buf || !*buf || *len == 0) return NULL;

    char *found = memchr(*buf, delim, *len);
    if (!found) return NULL;

    char *start = *buf;
    *tok_len = (size_t)(found - start);

    *buf = found + 1;
    *len -= (*tok_len + 1);
    return start;
}

// Dzieli gotową linię po separatorze `delim` (np. ' ' lub ':').
// Jeśli nie ma `delim`, zwraca resztę bufora jako ostatni token.
char *next_token_mem(char **buf, size_t *len, char delim, size_t *tok_len) {
    if (!buf || !*buf || *len == 0) return NULL;

    char *start = *buf;
    char *found = memchr(*buf, delim, *len);

    if (found) {
        *tok_len = (size_t)(found - start);
        *buf = found + 1;
        *len -= (*tok_len + 1);
    } else {
        *tok_len = *len;
        *buf += *len;
        *len = 0;
    }
    return start;
}
```

---

## 2. Porównywanie, przycinanie i parsowanie liczb (bez `\0`)

```c
bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Usuwa białe znaki z początku i końca wycinka
void mem_trim(char **ptr, size_t *len) {
    while (*len > 0 && is_ws((*ptr)[0])) {
        (*ptr)++;
        (*len)--;
    }
    while (*len > 0 && is_ws((*ptr)[*len - 1])) {
        (*len)--;
    }
}

// Porównuje wycinek (ptr, len) ze zwykłym stringiem zakończonym '\0'
bool mem_eq(const char *ptr, size_t len, const char *lit) {
    size_t lit_len = strlen(lit);
    return (len == lit_len) && (memcmp(ptr, lit, lit_len) == 0);
}

// Sprawdza, czy wycinek zaczyna się od podanego prefiksu
bool mem_starts_with(const char *ptr, size_t len, const char *prefix) {
    size_t p_len = strlen(prefix);
    return (len >= p_len) && (memcmp(ptr, prefix, p_len) == 0);
}

// Parsuje int z wycinka (ptr, len) ze sprawdzeniem przepełnienia
bool mem_parse_int(const char *ptr, size_t len, int *out) {
    if (!ptr || len == 0) return false;

    size_t i = 0;
    bool neg = false;
    if (ptr[0] == '-') {
        neg = true;
        i++;
    } else if (ptr[0] == '+') {
        i++;
    }
    if (i == len) return false;

    long val = 0;
    for (; i < len; i++) {
        char c = ptr[i];
        if (c < '0' || c > '9') return false;
        val = val * 10 + (c - '0');
        if (!neg && val > INT_MAX) return false;
        if (neg && -val < INT_MIN) return false;
    }

    *out = neg ? (int)(-val) : (int)val;
    return true;
}
```

---

## 3. Wzorzec użycia z `read()` / `poll()`

```c
char buf[4096];
size_t buf_len = 0;

// W pętli po wybudzeniu POLLIN:
ssize_t n = read(fd, buf + buf_len, sizeof(buf) - buf_len);
if (n > 0) {
    buf_len += (size_t)n;

    char *cursor = buf;
    size_t rem = buf_len;
    char *line;
    size_t line_len;

    // 1. Konsumujemy wszystkie pełne linie zakończone '\n'
    while ((line = next_chunk_strict(&cursor, &rem, '\n', &line_len)) != NULL) {
        mem_trim(&line, &line_len);
        if (line_len == 0) continue;

        // 2. Wyciągamy komendę i argument (np. "ADD 42")
        size_t cmd_len;
        char *cmd = next_token_mem(&line, &line_len, ' ', &cmd_len);

        if (mem_eq(cmd, cmd_len, "ADD")) {
            int val;
            mem_trim(&line, &line_len);
            if (mem_parse_int(line, line_len, &val)) {
                printf("Dodano: %d\n", val);
            }
        } else if (mem_eq(cmd, cmd_len, "QUIT")) {
            // obsługa rozłączenia
        }
    }

    // 3. Przesuwamy niekompletną końcówkę na początek bufora
    if (rem > 0 && cursor != buf) {
        memmove(buf, cursor, rem);
    }
    buf_len = rem;
}
```