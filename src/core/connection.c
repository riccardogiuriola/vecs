/*
 * Vex Project: Implementazione Gestione Connessione
 * (src/core/connection.c)
 *
 * MODIFICATO per Fase 6
 * Aggiunto include per sockaddr_storage (necessario in server.c)
 * e rimozione logica kqueue.
 */

#include "connection.h"
#include "server.h" 
#include "logger.h"
#include "vsp_parser.h"
#include "event_loop.h" // Aggiunto per coerenza
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

// Aggiunto per struct sockaddr_storage (usato in server.c)
#include <sys/socket.h> 

#define CONN_INITIAL_BUFFER_SIZE 1024

// Struct interna
struct vex_connection_s {
    int fd;
    struct vex_server_ctx_s *server;
    connection_state_t state;
    
    buffer_t *read_buf;  
    buffer_t *write_buf; 
    
    struct vsp_parser_s *parser;
};

vex_connection_t* connection_create(int fd, struct vex_server_ctx_s *server) {
    vex_connection_t *conn = malloc(sizeof(vex_connection_t));
    if (conn == NULL) {
        log_error("malloc fallito per vex_connection_t: %s", strerror(errno));
        return NULL;
    }

    conn->fd = fd;
    conn->server = server;
    conn->state = CONN_STATE_NEW; 

    conn->read_buf = buffer_create(CONN_INITIAL_BUFFER_SIZE);
    conn->write_buf = buffer_create(CONN_INITIAL_BUFFER_SIZE);
    conn->parser = vsp_parser_create();
    
    if (conn->read_buf == NULL || conn->write_buf == NULL || conn->parser == NULL) {
        log_error("Fallita creazione componenti connessione (fd: %d)", fd);
        if (conn->read_buf) buffer_destroy(conn->read_buf);
        if (conn->write_buf) buffer_destroy(conn->write_buf);
        if (conn->parser) vsp_parser_destroy(conn->parser);
        free(conn);
        return NULL;
    }
    
    return conn;
}

void connection_destroy(vex_connection_t *conn) {
    if (conn == NULL) return;

    // Imposta lo stato per evitare doppie chiusure
    // (es. se un errore accade in read e poi in write nello stesso loop)
    if(conn->state == CONN_STATE_CLOSING) return;
    conn->state = CONN_STATE_CLOSING;

    log_info("Distruzione connessione (fd: %d)", conn->fd);
    
    // server_remove_connection ora si occupa di rimuovere 
    // l'FD dal loop eventi (kqueue/epoll)
    server_remove_connection(conn->server, conn);

    close(conn->fd);
    buffer_destroy(conn->read_buf);
    buffer_destroy(conn->write_buf);
    vsp_parser_destroy(conn->parser);
    free(conn);
}


// --- Getters e Setters ---

int connection_get_fd(const vex_connection_t *conn) {
    return conn->fd;
}
struct vex_server_ctx_s* connection_get_server(const vex_connection_t *conn) {
    return conn->server;
}
buffer_t* connection_get_read_buffer(vex_connection_t *conn) {
    return conn->read_buf;
}
buffer_t* connection_get_write_buffer(vex_connection_t *conn) {
    return conn->write_buf;
}
connection_state_t connection_get_state(const vex_connection_t *conn) {
    return conn->state;
}
void connection_set_state(vex_connection_t *conn, connection_state_t state) {
    conn->state = state;
}
struct vsp_parser_s* connection_get_parser(const vex_connection_t *conn) {
    return conn->parser;
}