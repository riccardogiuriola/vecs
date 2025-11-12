/*
 * Vex Project: Header Gestione Connessione
 * (include/connection.h)
 */

#ifndef VEX_CONNECTION_H
#define VEX_CONNECTION_H

#include "buffer.h"

// Forward declaration
struct vex_server_ctx_s;
struct vsp_parser_s; // <-- AGGIUNTO (Fase 2)

// Stati possibili di una connessione
typedef enum {
    CONN_STATE_NEW,        
    CONN_STATE_READING,    
    CONN_STATE_PARSING,    
    CONN_STATE_WRITING,    
    CONN_STATE_CLOSING     
} connection_state_t;

// Handle opaco per la connessione
typedef struct vex_connection_s vex_connection_t;

/**
 * @brief Crea una nuova struttura di connessione per un client.
 * @param fd Il file descriptor del client.
 * @param server Il puntatore al contesto del server.
 * @return Un puntatore alla new connessione, o NULL in caso di fallimento.
 */
vex_connection_t* connection_create(int fd, struct vex_server_ctx_s *server);

/**
 * @brief Distrugge una connessione, chiude il fd, e libera i buffer.
 * @param conn La connessione da distruggere.
 */
void connection_destroy(vex_connection_t *conn);

// --- Getters ---
int connection_get_fd(const vex_connection_t *conn);
struct vex_server_ctx_s* connection_get_server(const vex_connection_t *conn);
buffer_t* connection_get_read_buffer(vex_connection_t *conn);
buffer_t* connection_get_write_buffer(vex_connection_t *conn);
connection_state_t connection_get_state(const vex_connection_t *conn);
struct vsp_parser_s* connection_get_parser(const vex_connection_t *conn); // <-- AGGIUNTO (Fase 2)

// --- Setters ---
void connection_set_state(vex_connection_t *conn, connection_state_t state);


#endif // VEX_CONNECTION_H