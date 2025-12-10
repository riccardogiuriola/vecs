/*
 * vecs Project: Header Socket
 * (include/socket.h)
 */

#ifndef vecs_SOCKET_H
#define vecs_SOCKET_H

/**
 * @brief Crea un socket TCP, lo imposta come non-bloccante,
 * abilita SO_REUSEADDR, e fa il bind sulla porta specificata.
 * * @param port La porta su cui fare il bind (come stringa, es. "6379").
 * @param backlog La dimensione della coda di listen.
 * @return Il file descriptor del socket di ascolto, o -1 in caso di errore.
 */
int socket_create_and_listen(const char *port, int backlog);

/**
 * @brief Imposta un file descriptor in modalità non-bloccante.
 * @param fd Il file descriptor.
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int socket_set_non_blocking(int fd);

#endif // vecs_SOCKET_H