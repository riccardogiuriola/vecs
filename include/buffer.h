/*
 * vex Project: Header Buffer Dinamico
 * (include/buffer.h)
 *
 * Un semplice buffer dinamico (stile sds di Redis).
 * NON è sicuro per i thread, pensato per l'uso single-threaded.
 */

#ifndef vex_BUFFER_H
#define vex_BUFFER_H

#include <stddef.h> // per size_t

// Handle opaco per il buffer
typedef struct buffer_s buffer_t;

/**
 * @brief Crea un nuovo buffer vuoto, preallocato con una capacità iniziale.
 * @param initial_capacity Capacità iniziale in byte.
 * @return Un puntatore al nuovo buffer, o NULL in caso di fallimento.
 */
buffer_t *buffer_create(size_t initial_capacity);

/**
 * @brief Distrugge un buffer e libera la sua memoria.
 * @param buf Il buffer da distruggere.
 */
void buffer_destroy(buffer_t *buf);

/**
 * @brief Aggiunge dati alla fine del buffer, espandendolo se necessario.
 * @param buf Il buffer.
 * @param data Puntatore ai dati da aggiungere.
 * @param len Lunghezza dei dati da aggiungere.
 * @return 0 in caso di successo, -1 in caso di fallimento (es. allocazione).
 */
int buffer_append(buffer_t *buf, const void *data, size_t len);

/**
 * @brief Rimuove dati dall'inizio del buffer (consuma).
 * Sposta la memoria rimanente all'inizio.
 * @param buf Il buffer.
 * @param len Lunghezza dei dati da consumare.
 */
void buffer_consume(buffer_t *buf, size_t len);

/**
 * @brief Restituisce un puntatore ai dati nel buffer.
 * @param buf Il buffer.
 * @return Puntatore all'inizio dei dati.
 */
const char *buffer_data(const buffer_t *buf);

/**
 * @brief Restituisce la quantità di dati attualmente nel buffer.
 * @param buf Il buffer.
 * @return Lunghezza dei dati in byte.
 */
size_t buffer_len(const buffer_t *buf);

/**
 * @brief Restituisce lo spazio rimanente prima di una riallocazione.
 * @param buf Il buffer.
 * @return Spazio disponibile in byte.
 */
size_t buffer_available(const buffer_t *buf);

/**
 * @brief Pulisce il buffer (resetta la lunghezza a 0) senza liberare la memoria.
 * @param buf Il buffer.
 */
void buffer_clear(buffer_t *buf);

#endif // vex_BUFFER_H