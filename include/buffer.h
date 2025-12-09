/*
 * Vex Project: Header Buffer Dinamico
 * (include/buffer.h)
 */

#ifndef VEX_BUFFER_H
#define VEX_BUFFER_H

#include <stddef.h> // per size_t
#include <sys/types.h> // per ssize_t

// Handle opaco per il buffer
typedef struct buffer_s buffer_t;

/**
 * @brief Crea un nuovo buffer vuoto, preallocato con una capacità iniziale.
 * @param initial_capacity Capacità iniziale in byte.
 * @return Un puntatore al nuovo buffer, o NULL in caso di fallimento.
 */
buffer_t* buffer_create(size_t initial_capacity);

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
int buffer_append_data(buffer_t *buf, const void *data, size_t len);

/**
 * @brief Aggiunge una stringa C (terminata da null) alla fine del buffer.
 * @param buf Il buffer.
 * @param str La stringa da aggiungere (il terminatore nullo non viene copiato).
 * @return 0 in caso di successo, -1 in caso di fallimento.
 */
int buffer_append_string(buffer_t *buf, const char *str);


/**
 * @brief Rimuove dati dall'inizio del buffer (consuma).
 * Sposta la memoria rimanente all'inizio.
 * @param buf Il buffer.
 * @param len Lunghezza dei dati da consumare.
 */
void buffer_consume(buffer_t *buf, size_t len);

/**
 * @brief Restituisce un puntatore all'inizio dei dati nel buffer.
 * @param buf Il buffer.
 * @return Puntatore all'inizio dei dati.
 */
const void* buffer_peek(const buffer_t *buf);

/**
 * @brief Restituisce la quantità di dati attualmente nel buffer.
 * @param buf Il buffer.
 * @return Lunghezza dei dati in byte.
 */
size_t buffer_len(const buffer_t *buf);

/**
 * @brief Cerca la prima occorrenza di \r\n nel buffer.
 * @param buf Il buffer.
 * @return Un puntatore all'inizio di \r\n, o NULL se non trovato.
 */
char* buffer_find_crlf(buffer_t *buf);


/**
 * @brief Legge dati da un file descriptor e li aggiunge al buffer.
 * Gestisce la crescita del buffer.
 * @param buf Il buffer in cui scrivere.
 * @param fd Il file descriptor da cui leggere.
 * @return Numero di byte letti, 0 per EOF, -1 per errore (errno è impostato).
 */
ssize_t buffer_read_from_fd(buffer_t *buf, int fd);

/**
 * @brief Scrive dati da un buffer a un file descriptor.
 * Consuma i dati scritti dal buffer.
 * @param buf Il buffer da cui leggere.
 * @param fd Il file descriptor in cui scrivere.
 * @return Numero di byte scritti, -1 per errore (errno è impostato).
 */
ssize_t buffer_write_to_fd(buffer_t *buf, int fd);


#endif // VEX_BUFFER_H