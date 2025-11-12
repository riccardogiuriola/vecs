/*
 * vecs Project: Implementazione Server (Reactor Kqueue)
 * (src/core/server.c)
 */

#include "server.h"
#include "logger.h"
#include "socket.h"
#include "connection.h" // <-- AGGIUNTO
#include "buffer.h"     // <-- AGGIUNTO

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

// --- Include specifici per kqueue (macOS/BSD) ---
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/time.h>
// ---

#define MAX_EVENTS 64     // Numero massimo di eventi da processare per ciclo
#define LISTEN_BACKLOG 511
#define MAX_FD FD_SETSIZE // Limite massimo di connessioni (da sys/select.h, es. 1024)

// --- Implementazione del "Context Handle" opaco ---
struct vecs_server_ctx_s {
    int listen_fd; // Socket di ascolto
    int kq_fd;     // File descriptor di kqueue
    
    // Array per tracciare tutte le connessioni attive.
    // L'indice è il file descriptor. Approccio O(1) per lookup.
    vecs_connection_t *connections[MAX_FD];
};
// --- Fine implementazione Handle ---


// Funzione helper per registrare eventi con kqueue (ora pubblica via server.h)
void server_register_event(vecs_server_t *server, int fd, int16_t filter, uint16_t flags, void *udata) {
    struct kevent change;
    
    // NOTA PER LINUX (epoll):
    // L'equivalente sarebbe una struct epoll_event e una chiamata a epoll_ctl().
    // struct epoll_event ev;
    // ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    // ev.data.ptr = udata;
    // epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    EV_SET(&change, fd, filter, flags, 0, 0, udata);
    
    if (kevent(server->kq_fd, &change, 1, NULL, 0, NULL) == -1) {
        log_fatal("kevent() fallito durante la registrazione (fd: %d): %s", fd, strerror(errno));
    }
}


// Funzione helper per gestire le nuove connessioni in entrata
static void server_handle_new_connection(vecs_server_t *server) {
    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &addr_len);

        if (client_fd == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                break; // Nessuna nuova connessione in attesa
            } else {
                log_error("accept() fallito: %s", strerror(errno));
                break;
            }
        }

        // Controlla se abbiamo superato il limite di connessioni
        if (client_fd >= MAX_FD) {
            log_error("Raggiunto limite FD (MAX_FD: %d). Rifiuto connessione (fd: %d).", MAX_FD, client_fd);
            close(client_fd);
            continue;
        }

        // Imposta il socket del client come non-bloccante
        if (socket_set_non_blocking(client_fd) == -1) {
            log_error("Impossibile impostare non-bloccante per il client fd %d", client_fd);
            close(client_fd);
            continue;
        }

        // --- Logica Fase 1 ---
        // Crea la struttura della connessione
        vecs_connection_t *conn = connection_create(client_fd, server);
        if (conn == NULL) {
            log_error("Fallita creazione vecs_connection_t per fd %d", client_fd);
            close(client_fd);
            continue;
        }
        
        // Salva la connessione nell'array del server
        server->connections[client_fd] = conn;
        log_info("Nuovo client connesso (fd: %d)", client_fd);

        // Aggiunge il client al kqueue per monitorare gli eventi di LETTURA
        // Ora usiamo 'conn' come udata!
        server_register_event(server, client_fd, 
                              EVFILT_READ, EV_ADD | EV_ENABLE, 
                              (void*)conn);
    }
}

// Funzione helper per gestire i dati in lettura da un client (Fase 1/2)
static void server_handle_client_read(vecs_connection_t *conn) {
    if (conn == NULL) return;

    int fd = connection_get_fd(conn);
    buffer_t *read_buf = connection_get_read_buffer(conn);
    
    // Leggi i dati in un buffer temporaneo
    char temp_buf[4096];
    ssize_t nread = read(fd, temp_buf, sizeof(temp_buf));

    if (nread > 0) {
        // Dati ricevuti, aggiungili al buffer di lettura della connessione
        if (buffer_append(read_buf, temp_buf, nread) == -1) {
            log_error("Fallita append al read_buf (fd: %d). Chiudo connessione.", fd);
            connection_destroy(conn);
            return;
        }
        
        log_info("Ricevuti %zd byte dal client (fd: %d)", nread, fd);
        connection_set_state(conn, CONN_STATE_PARSING);

        // TODO (Fase 2): Chiamare il parser VSP qui.
        // vsp_parser_execute(conn, buffer_data(read_buf), buffer_len(read_buf));
        // Per ora, l'MVP della Fase 1 non fa nulla con i dati.
        
        // --- TEST ECHO (da rimuovere in Fase 2) ---
        // Per testare Fase 1, copiamo i dati letti nel buffer di scrittura
        // per creare un "echo server".
        buffer_t *write_buf = connection_get_write_buffer(conn);
        if (buffer_append(write_buf, temp_buf, nread) == -1) {
             log_error("Fallita append al write_buf (fd: %d).", fd);
        } else {
             // Dati pronti per la scrittura, abilita EVFILT_WRITE
             server_register_event(connection_get_server(conn), fd,
                                   EVFILT_WRITE, EV_ADD | EV_ENABLE,
                                   (void*)conn);
        }
        buffer_clear(read_buf); // Puliamo il read buffer (solo per l'echo test)
        // --- FINE TEST ECHO ---


    } else if (nread == 0) {
        // Connessione chiusa dal client (EOF)
        log_info("Client (fd: %d) ha chiuso la connessione (EOF)", fd);
        connection_destroy(conn); // Questo gestirà la disconnessione
    } else { // nread == -1
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Nessun dato da leggere (normale per non-bloccante)
        } else {
            // Errore reale
            log_error("Errore read() su fd %d: %s. Chiudo connessione.", fd, strerror(errno));
            connection_destroy(conn);
        }
    }
}

// Funzione helper per scrivere dati a un client (Fase 1/2)
static void server_handle_client_write(vecs_connection_t *conn) {
    if (conn == NULL) return;

    int fd = connection_get_fd(conn);
    buffer_t *write_buf = connection_get_write_buffer(conn);
    size_t data_len = buffer_len(write_buf);

    if (data_len == 0) {
        // Nulla da scrivere (non dovrebbe accadere se l'evento è attivo)
        return;
    }

    ssize_t nwritten = write(fd, buffer_data(write_buf), data_len);

    if (nwritten > 0) {
        // Scrittura parziale o completa
        buffer_consume(write_buf, nwritten);
        log_info("Scritti %zd byte al client (fd: %d). Rimasti %zu.", nwritten, fd, buffer_len(write_buf));

        if (buffer_len(write_buf) == 0) {
            // Buffer vuoto, non siamo più interessati a scrivere.
            // DISABILITA EVFILT_WRITE (fondamentale per evitare 100% CPU)
            server_register_event(connection_get_server(conn), fd,
                                  EVFILT_WRITE, EV_DELETE,
                                  (void*)conn);
        }
    } else if (nwritten == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Il buffer di scrittura del kernel è pieno. Riproveremo.
            // L'evento EVFILT_WRITE rimane attivo.
        } else {
            // Errore reale
            log_error("Errore write() su fd %d: %s. Chiudo connessione.", fd, strerror(errno));
            connection_destroy(conn);
        }
    }
}


// --- Implementazione delle funzioni pubbliche ---

vecs_server_t* server_create(const char *port) {
    vecs_server_t *server = malloc(sizeof(vecs_server_t));
    if (server == NULL) {
        log_error("malloc fallito per vecs_server_t: %s", strerror(errno));
        return NULL;
    }

    // Inizializza l'array delle connessioni a NULL
    memset(server->connections, 0, sizeof(server->connections));

    server->listen_fd = socket_create_and_listen(port, LISTEN_BACKLOG);
    if (server->listen_fd == -1) {
        free(server);
        return NULL;
    }

    // Crea l'istanza kqueue
    server->kq_fd = kqueue();
    if (server->kq_fd == -1) {
        log_fatal("kqueue() fallito: %s", strerror(errno));
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    // Registra il socket di ascolto.
    // Usiamo 'server' come udata per distinguerlo dalle connessioni client.
    server_register_event(server, server->listen_fd, 
                          EVFILT_READ, EV_ADD | EV_ENABLE, 
                          (void*)server); // <-- udata è il server stesso

    log_info("vecs server inizializzato. In ascolto sulla porta %s (fd: %d)", port, server->listen_fd);
    return server;
}

void server_run(vecs_server_t *server) {
    struct kevent active_events[MAX_EVENTS];
    
    log_info("Avvio del loop eventi (Reactor)...");
    
    while (1) {
        int num_events = kevent(server->kq_fd, NULL, 0, active_events, MAX_EVENTS, NULL);

        if (num_events == -1) {
            if (errno == EINTR) continue;
            log_fatal("kevent() wait fallito: %s", strerror(errno));
        }

        for (int i = 0; i < num_events; i++) {
            struct kevent *e = &active_events[i];
            
            // Estrai udata
            void *udata = e->udata;

            // 1. Evento sul socket di ascolto
            if (udata == (void*)server) {
                if (e->flags & EV_EOF) {
                    log_fatal("Socket di ascolto chiuso inaspettatamente!");
                }
                server_handle_new_connection(server);
            } 
            // 2. Evento su un client (udata è vecs_connection_t*)
            else {
                vecs_connection_t *conn = (vecs_connection_t*)udata;

                // Evento di disconnessione (EOF)
                if (e->flags & EV_EOF) {
                    log_info("Client (fd: %d) disconnesso (evento EV_EOF)", connection_get_fd(conn));
                    connection_destroy(conn);
                }
                // Evento di lettura
                else if (e->filter == EVFILT_READ) {
                    server_handle_client_read(conn);
                }
                // Evento di scrittura
                else if (e->filter == EVFILT_WRITE) {
                    server_handle_client_write(conn);
                }
            }
        }
    }
}

void server_destroy(vecs_server_t *server) {
    if (server == NULL) {
        return;
    }

    log_info("Spegnimento del server...");
    close(server->listen_fd);
    close(server->kq_fd);

    // Chiude tutte le connessioni client rimanenti
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (server->connections[fd] != NULL) {
            // Nota: connection_destroy modifica server->connections[fd] a NULL
            // tramite server_remove_connection, quindi è sicuro.
            connection_destroy(server->connections[fd]);
        }
    }

    free(server);
}

// Funzione interna chiamata da connection_destroy
void server_remove_connection(vecs_server_t *server, struct vecs_connection_s *conn) {
    if (conn == NULL) return;
    int fd = connection_get_fd(conn);
    if (fd >= 0 && fd < MAX_FD) {
        server->connections[fd] = NULL;
    }
}