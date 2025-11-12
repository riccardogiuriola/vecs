/*
 * vecs Project: Implementazione Gestione Connessione
 * (src/core/connection.c)
 */

#include "connection.h"
#include "server.h" // Include server.h per la definizione completa di vecs_server_ctx_s
#include "logger.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

// Capacità iniziale dei buffer di I/O
#define CONN_INITIAL_BUFFER_SIZE 1024

// Struct interna (non visibile dall'header)
struct vecs_connection_s
{
    int fd;
    struct vecs_server_ctx_s *server; // Puntatore al server proprietario
    connection_state_t state;

    buffer_t *read_buf;  // Buffer per i dati letti dal client
    buffer_t *write_buf; // Buffer per i dati da scrivere al client

    // Qui andranno i dati specifici del parser VSP (Fase 2)
};

vecs_connection_t *connection_create(int fd, struct vecs_server_ctx_s *server)
{
    vecs_connection_t *conn = malloc(sizeof(vecs_connection_t));
    if (conn == NULL)
    {
        log_error("malloc fallito per vecs_connection_t: %s", strerror(errno));
        return NULL;
    }

    conn->fd = fd;
    conn->server = server;
    conn->state = CONN_STATE_NEW; // Stato iniziale

    conn->read_buf = buffer_create(CONN_INITIAL_BUFFER_SIZE);
    if (conn->read_buf == NULL)
    {
        log_error("Impossibile creare read_buf per conn (fd: %d)", fd);
        free(conn);
        return NULL;
    }

    conn->write_buf = buffer_create(CONN_INITIAL_BUFFER_SIZE);
    if (conn->write_buf == NULL)
    {
        log_error("Impossibile creare write_buf per conn (fd: %d)", fd);
        buffer_destroy(conn->read_buf);
        free(conn);
        return NULL;
    }

    // log_debug("Creata connessione (fd: %d)", fd); // Debug
    return conn;
}

void connection_destroy(vecs_connection_t *conn)
{
    if (conn == NULL)
        return;

    log_info("Distruzione connessione (fd: %d)", conn->fd);

    // Rimuove la connessione dall'array del server
    // (Il server deve essere incluso per questo)
    server_remove_connection(conn->server, conn);

    close(conn->fd);
    buffer_destroy(conn->read_buf);
    buffer_destroy(conn->write_buf);
    free(conn);
}

// --- Getters e Setters ---

int connection_get_fd(const vecs_connection_t *conn)
{
    return conn->fd;
}

struct vecs_server_ctx_s *connection_get_server(const vecs_connection_t *conn)
{
    return conn->server;
}

buffer_t *connection_get_read_buffer(vecs_connection_t *conn)
{
    return conn->read_buf;
}

buffer_t *connection_get_write_buffer(vecs_connection_t *conn)
{
    return conn->write_buf;
}

connection_state_t connection_get_state(const vecs_connection_t *conn)
{
    return conn->state;
}

void connection_set_state(vecs_connection_t *conn, connection_state_t state)
{
    conn->state = state;
}