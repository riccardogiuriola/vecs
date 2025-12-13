/*
 * Vecs Project: Header Gestione Connessione
 * (include/connection.h)
 */
#ifndef VECS_CONNECTION_H
#define VECS_CONNECTION_H

#include <stddef.h> // Per size_t
#include <stdint.h>

/*
 * Handle opachi.
 * Usiamo la forward declaration corretta per vecs_server_t
 * che corrisponde a quella in server.h
 */
typedef struct vecs_server_s vecs_server_t;
typedef struct vecs_connection_s vecs_connection_t;
typedef struct vsp_parser_s vsp_parser_t;
typedef struct buffer_s buffer_t;

/**
 * @brief Stato di una connessione (per scritture/chiusure differite)
 */
typedef enum
{
    STATE_READING, // Stato normale
    STATE_WRITING, // Non usato attivamente, ma utile per la logica
    STATE_CLOSING  // In attesa di chiusura dopo aver svuotato il write_buffer
} vecs_connection_state_t;

/**
 * @brief Crea una nuova struttura di connessione.
 * * @param server Il puntatore all'istanza del server.
 * @param fd Il file descriptor del client.
 * @return Un puntatore alla nuova connessione, o NULL.
 */
vecs_connection_t *connection_create(vecs_server_t *server, int fd);

/**
 * @brief Distrugge una connessione e libera tutte le sue risorse.
 * Chiude l'FD, libera i buffer e il parser.
 * * @param conn La connessione da distruggere.
 */
void connection_destroy(vecs_connection_t *conn);

// --- Getters per l'accesso opaco ---

int connection_get_fd(vecs_connection_t *conn);
vecs_server_t *connection_get_server(vecs_connection_t *conn);
buffer_t *connection_get_read_buffer(vecs_connection_t *conn);
buffer_t *connection_get_write_buffer(vecs_connection_t *conn);
vsp_parser_t *connection_get_parser(vecs_connection_t *conn);
vecs_connection_state_t connection_get_state(vecs_connection_t *conn);
uint64_t connection_get_id(vecs_connection_t *conn);

// --- Setters per l'accesso opaco ---

void connection_set_state(vecs_connection_t *conn, vecs_connection_state_t state);

#endif // VECS_CONNECTION_H