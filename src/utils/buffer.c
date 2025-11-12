/*
 * vecs Project: Implementazione Buffer Dinamico
 * (src/utils/buffer.c)
 */

#include "buffer.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Valore minimo di allocazione
#define BUFFER_INITIAL_CAPACITY 64

// Struct interna (non visibile dall'header)
struct buffer_s {
    char *data;     // Puntatore alla memoria allocata
    size_t len;     // Dati attualmente utilizzati
    size_t capacity; // Memoria totale allocata
};

// Funzione helper per riallocare il buffer
static int buffer_grow(buffer_t *buf, size_t min_needed) {
    if (buffer_available(buf) >= min_needed) {
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

int buffer_append(buffer_t *buf, const void *data, size_t len) {
    if (buffer_grow(buf, len) == -1) {
        return -1; // Fallimento allocazione
    }

    // Aggiunge i nuovi dati
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
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

const char* buffer_data(const buffer_t *buf) {
    return buf->data;
}

size_t buffer_len(const buffer_t *buf) {
    return buf->len;
}

size_t buffer_available(const buffer_t *buf) {
    return buf->capacity - buf->len;
}

void buffer_clear(buffer_t *buf) {
    buf->len = 0;
}