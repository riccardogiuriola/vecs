/*
 * Vecs Project: Implementazione Buffer Dinamico
 * (src/utils/buffer.c)
 *
 * --- AGGIORNATO (Fase 3/Fix) ---
 * Aggiunte funzioni di I/O e helper.
 */

#include "buffer.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h> // Per read() e write()
#include <sys/uio.h> // Per readv/writev (opzionale ma veloce)

// Valore minimo di allocazione
#define BUFFER_INITIAL_CAPACITY 64
// Quanto spazio riservare per le letture
#define BUFFER_READ_SIZE 4096

// Struct interna (non visibile dall'header)
struct buffer_s {
    char *data;     // Puntatore alla memoria allocata
    size_t len;     // Dati attualmente utilizzati
    size_t capacity; // Memoria totale allocata
};

// Funzione helper per riallocare il buffer
static int buffer_grow(buffer_t *buf, size_t min_needed) {
    if (buf->capacity - buf->len >= min_needed) {
        return 0; // Spazio sufficiente
    }

    size_t new_capacity = buf->capacity;
    // Strategia di crescita "raddoppia" (stile C++ vector / Redis sds)
    while (new_capacity < buf->len + min_needed) {
        new_capacity = (new_capacity == 0) ? BUFFER_INITIAL_CAPACITY : new_capacity * 2;
    }

    char *new_data = realloc(buf->data, new_capacity);
    if (new_data == NULL) {
        log_error("realloc fallito per il buffer: %s", strerror(errno));
        return -1;
    }

    buf->data = new_data;
    buf->capacity = new_capacity;
    return 0;
}

// --- Funzioni Pubbliche ---

buffer_t* buffer_create(size_t initial_capacity) {
    buffer_t *buf = malloc(sizeof(buffer_t));
    if (buf == NULL) {
        log_error("malloc fallito per buffer_t: %s", strerror(errno));
        return NULL;
    }

    size_t capacity = (initial_capacity > 0) ? initial_capacity : BUFFER_INITIAL_CAPACITY;
    
    buf->data = malloc(capacity);
    if (buf->data == NULL) {
        log_error("malloc fallito per i dati del buffer: %s", strerror(errno));
        free(buf);
        return NULL;
    }

    buf->len = 0;
    buf->capacity = capacity;
    return buf;
}

void buffer_destroy(buffer_t *buf) {
    if (buf == NULL) return;
    free(buf->data);
    free(buf);
}

int buffer_append_data(buffer_t *buf, const void *data, size_t len) {
    if (buffer_grow(buf, len) == -1) {
        return -1; // Fallimento allocazione
    }

    // Aggiunge i nuovi dati
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
}

int buffer_append_string(buffer_t *buf, const char *str) {
    return buffer_append_data(buf, str, strlen(str));
}

void buffer_consume(buffer_t *buf, size_t len) {
    if (len == 0) return;

    if (len >= buf->len) {
        // Consuma tutto
        buf->len = 0;
        return;
    }

    // Consuma parzialmente: sposta la memoria rimanente all'inizio
    size_t remaining = buf->len - len;
    memmove(buf->data, buf->data + len, remaining);
    buf->len = remaining;
}

const void* buffer_peek(const buffer_t *buf) {
    return buf->data;
}

size_t buffer_len(const buffer_t *buf) {
    return buf->len;
}

char* buffer_find_crlf(buffer_t *buf) {
    if (buf->len < 2) return NULL;
    // Cerca \r\n
    // memmem è GNU, strnstr è BSD/POSIX. memchr è C standard.
    // Usiamo un loop manuale per massima portabilità
    for (size_t i = 0; i < buf->len - 1; i++) {
        if (buf->data[i] == '\r' && buf->data[i+1] == '\n') {
            return buf->data + i;
        }
    }
    return NULL;
}


ssize_t buffer_read_from_fd(buffer_t *buf, int fd) {
    // Assicurati che ci sia spazio per leggere (almeno BUFFER_READ_SIZE)
    if (buffer_grow(buf, BUFFER_READ_SIZE) == -1) {
        errno = ENOMEM;
        return -1;
    }
    
    // Leggi direttamente nello spazio disponibile del buffer
    ssize_t nread = read(fd, buf->data + buf->len, buf->capacity - buf->len);
    
    if (nread > 0) {
        buf->len += nread; // Aggiorna la lunghezza
    }
    
    return nread; // Restituisce nread, 0 (EOF), or -1 (errore)
}

ssize_t buffer_write_to_fd(buffer_t *buf, int fd) {
    if (buf->len == 0) {
        return 0; // Niente da scrivere
    }

    ssize_t nwritten = write(fd, buf->data, buf->len);

    if (nwritten > 0) {
        // Consuma i dati che abbiamo scritto con successo
        buffer_consume(buf, nwritten);
    }

    return nwritten; // Restituisce nwritten or -1 (errore)
}