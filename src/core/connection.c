/*
 * Vecs Project: Implementazione Gestione Connessione
 * (src/core/connection.c)
 */

#include "connection.h"
#include "server.h" 
#include "logger.h"
#include "vsp_parser.h" 
#include "buffer.h"     
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>


#define CONN_INITIAL_BUFFER_SIZE 1024

// Struct interna
struct vecs_connection_s {
    int fd;
    vecs_server_t *server;
    vecs_connection_state_t state;
    
    buffer_t *read_buf;  
    buffer_t *write_buf; 
    
    vsp_parser_t *parser;
};

vecs_connection_t* connection_create(vecs_server_t *server, int fd) {
    vecs_connection_t *conn = malloc(sizeof(vecs_connection_t));
    if (conn == NULL) {
        log_error("malloc fallito per vecs_connection_t: %s", strerror(errno));
        return NULL;
    }

    conn->fd = fd;
    conn->server = server;
    conn->state = STATE_READING;

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

void connection_destroy(vecs_connection_t *conn) {
    if (conn == NULL) return;

    // Prevenzione doppia chiusura
    if (conn->fd == -1) return;

    log_debug("Distruzione connessione (fd: %d)", conn->fd);
    
    // server_remove_connection (in server.c) si occupa di 
    // rimuovere da epoll e dal pool.
    
    close(conn->fd);
    conn->fd = -1; // Marca come chiuso

    buffer_destroy(conn->read_buf);
    buffer_destroy(conn->write_buf);
    vsp_parser_destroy(conn->parser);
    free(conn);
}


// --- Getters e Setters ---

int connection_get_fd(vecs_connection_t *conn) {
    return conn->fd;
}
vecs_server_t* connection_get_server(vecs_connection_t *conn) {
    return conn->server;
}
buffer_t* connection_get_read_buffer(vecs_connection_t *conn) {
    return conn->read_buf;
}
buffer_t* connection_get_write_buffer(vecs_connection_t *conn) {
    return conn->write_buf;
}

vsp_parser_t* connection_get_parser(vecs_connection_t *conn) {
    return conn->parser;
}

vecs_connection_state_t connection_get_state(vecs_connection_t *conn) {
    return conn->state;
}
void connection_set_state(vecs_connection_t *conn, vecs_connection_state_t state) {
    conn->state = state;
}