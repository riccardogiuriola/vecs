/*
 * vecs Project: Header Gestione Connessione
 * (include/connection.h)
 *
 * Rappresenta lo stato di una singola connessione client.
 */

#ifndef vecs_CONNECTION_H
#define vecs_CONNECTION_H

#include "buffer.h"

// Forward declaration per evitare include circolare con server.h
// Vogliamo che la connessione conosca il server, e il server le connessioni.
struct vecs_server_ctx_s;

// Stati possibili di una connessione (per la state machine)
typedef enum
{
    CONN_STATE_NEW,     // Appena connesso, in attesa di dati
    CONN_STATE_READING, // Leggendo attivamente
    CONN_STATE_PARSING, // Dati nel buffer, pronti per il parser VSP
    CONN_STATE_WRITING, // Scrivendo attivamente risposta
    CONN_STATE_CLOSING  // In attesa di chiusura
} connection_state_t;

// Handle opaco per la connessione
typedef struct vecs_connection_s vecs_connection_t;

/**
 * @brief Crea una nuova struttura di connessione per un client.
 * @param fd Il file descriptor del client.
 * @param server Il puntatore al contesto del server (per accedere al kqueue, ecc.).
 * @return Un puntatore alla nuova connessione, o NULL in caso di fallimento.
 */
vecs_connection_t *connection_create(int fd, struct vecs_server_ctx_s *server);

/**
 * @brief Distrugge una connessione, chiude il fd, e libera i buffer.
 * @param conn La connessione da distruggere.
 */
void connection_destroy(vecs_connection_t *conn);

// --- Getters per i campi ---
// Usiamo i getter per mantenere l'incapsulamento

int connection_get_fd(const vecs_connection_t *conn);
struct vecs_server_ctx_s *connection_get_server(const vecs_connection_t *conn);
buffer_t *connection_get_read_buffer(vecs_connection_t *conn);
buffer_t *connection_get_write_buffer(vecs_connection_t *conn);
connection_state_t connection_get_state(const vecs_connection_t *conn);

// --- Setters per lo stato ---
void connection_set_state(vecs_connection_t *conn, connection_state_t state);

#endif // vecs_CONNECTION_H