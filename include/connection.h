/*
 * Vex Project: Header Gestione Connessione
 * (include/connection.h)
 */
#ifndef VEX_CONNECTION_H
#define VEX_CONNECTION_H

#include <stddef.h> // Per size_t

/*
 * Handle opachi.
 * Usiamo la forward declaration corretta per vex_server_t
 * che corrisponde a quella in server.h
 */
typedef struct vex_server_s vex_server_t;
typedef struct vex_connection_s vex_connection_t;
typedef struct vsp_parser_s vsp_parser_t;
typedef struct buffer_s buffer_t;

/**
 * @brief Stato di una connessione (per scritture/chiusure differite)
 */
typedef enum {
    STATE_READING,  // Stato normale
    STATE_WRITING,  // Non usato attivamente, ma utile per la logica
    STATE_CLOSING   // In attesa di chiusura dopo aver svuotato il write_buffer
} vex_connection_state_t;


/**
 * @brief Crea una nuova struttura di connessione.
 * * @param server Il puntatore all'istanza del server.
 * @param fd Il file descriptor del client.
 * @return Un puntatore alla nuova connessione, o NULL.
 */
vex_connection_t* connection_create(vex_server_t *server, int fd);

/**
 * @brief Distrugge una connessione e libera tutte le sue risorse.
 * Chiude l'FD, libera i buffer e il parser.
 * * @param conn La connessione da distruggere.
 */
void connection_destroy(vex_connection_t *conn);

// --- Getters per l'accesso opaco ---

int connection_get_fd(vex_connection_t *conn);
vex_server_t* connection_get_server(vex_connection_t *conn);
buffer_t* connection_get_read_buffer(vex_connection_t *conn);
buffer_t* connection_get_write_buffer(vex_connection_t *conn);
vsp_parser_t* connection_get_parser(vex_connection_t *conn);
vex_connection_state_t connection_get_state(vex_connection_t *conn);

// --- Setters per l'accesso opaco ---

void connection_set_state(vex_connection_t *conn, vex_connection_state_t state);


#endif // VEX_CONNECTION_H