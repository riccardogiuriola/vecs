/*
 * Vex Project: Implementazione Gestione Connessione
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
struct vex_connection_s {
    int fd;
    vex_server_t *server;
    vex_connection_state_t state;
    
    buffer_t *read_buf;  
    buffer_t *write_buf; 
    
    vsp_parser_t *parser;
};

vex_connection_t* connection_create(vex_server_t *server, int fd) {
    vex_connection_t *conn = malloc(sizeof(vex_connection_t));
    if (conn == NULL) {
        log_error("malloc fallito per vex_connection_t: %s", strerror(errno));
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

void connection_destroy(vex_connection_t *conn) {
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

int connection_get_fd(vex_connection_t *conn) {
    return conn->fd;
}
vex_server_t* connection_get_server(vex_connection_t *conn) {
    return conn->server;
}
buffer_t* connection_get_read_buffer(vex_connection_t *conn) {
    return conn->read_buf;
}
buffer_t* connection_get_write_buffer(vex_connection_t *conn) {
    return conn->write_buf;
}

vsp_parser_t* connection_get_parser(vex_connection_t *conn) {
    return conn->parser;
}

vex_connection_state_t connection_get_state(vex_connection_t *conn) {
    return conn->state;
}
void connection_set_state(vex_connection_t *conn, vex_connection_state_t state) {
    conn->state = state;
}