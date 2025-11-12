/*
 * Vex Project: Implementazione Server
 * (src/core/server.c)
 *
 * MODIFICATO per Fase 6 (Astrazione Event Loop)
 * Questo file ora è agnostico rispetto a kqueue/epoll.
 */

#include "server.h"
#include "logger.h"
#include "socket.h"
#include "connection.h" 
#include "buffer.h"     
#include "vsp_parser.h"
#include "event_loop.h" // <-- AGGIUNTA API ASTRAZIONE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h> 
#include <sys/socket.h> // <-- CORREZIONE: Aggiunto per sockaddr_storage e socklen_t

/*
 * RIMOSSI: Tutti gli include specifici di piattaforma (kqueue)
 * <sys/types.h>, <sys/socket.h>, <sys/event.h>, <sys/time.h>
 * Ora sono in event_kqueue.c o event_epoll.c
 */

#define MAX_EVENTS 64     
#define LISTEN_BACKLOG 511
#define MAX_FD FD_SETSIZE 

// --- Implementazione del "Context Handle" opaco ---
struct vex_server_ctx_s {
    int listen_fd; // Socket di ascolto
    
    vex_event_loop_t *loop; // <-- Puntatore al loop astratto
    
    vex_connection_t *connections[MAX_FD];
};
// --- Fine implementazione Handle ---


/*
 * RIMOSSA: server_register_event
 * Ora usiamo le funzioni el_* (es. el_enable_write)
 */


// Funzione helper per gestire le nuove connessioni in entrata
static void server_handle_new_connection(vex_server_t *server) {
    // NOTA: Con EPOLLET (Edge Triggered), dobbiamo ciclare accept()
    // finché non restituisce EWOULDBLOCK.
    while (1) { 
        // Non abbiamo più bisogno di <sys/socket.h> qui perché
        // è già incluso in socket.h (che include socket.c)
        // Ma per pulizia, e per socklen_t, lo aggiungiamo.
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &addr_len);

        if (client_fd == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                break; // Nessuna nuova connessione (necessario per EPOLLET)
            } else {
                log_error("accept() fallito: %s", strerror(errno));
                break;
            }
        }

        if (client_fd >= MAX_FD) {
            log_error("Raggiunto limite FD. Rifiuto connessione (fd: %d).", client_fd);
            close(client_fd);
            continue;
        }

        if (socket_set_non_blocking(client_fd) == -1) {
            close(client_fd);
            continue;
        }

        vex_connection_t *conn = connection_create(client_fd, server);
        if (conn == NULL) {
            close(client_fd);
            continue;
        }
        
        server->connections[client_fd] = conn;
        log_info("Nuovo client connesso (fd: %d)", client_fd);

        // Aggiunge il client al loop eventi (astratto)
        if (el_add_fd_read(server->loop, client_fd, (void*)conn) == -1) {
            log_error("Fallita registrazione client (fd: %d) al loop eventi.", client_fd);
            connection_destroy(conn); // Questo pulisce tutto
        }
    }
}

// Funzione helper per gestire i dati in lettura da un client
static void server_handle_client_read(vex_connection_t *conn) {
    if (conn == NULL) return;

    int fd = connection_get_fd(conn);
    buffer_t *read_buf = connection_get_read_buffer(conn);
    
    // NOTA: Con EPOLLET (Edge Triggered), dobbiamo leggere
    // finché read() non restituisce EWOULDBLOCK.
    while(1) {
        char temp_buf[4096];
        ssize_t nread = read(fd, temp_buf, sizeof(temp_buf));

        if (nread > 0) {
            // Dati ricevuti
            if (buffer_append(read_buf, temp_buf, nread) == -1) {
                log_error("Fallita append al read_buf (fd: %d). Chiudo.", fd);
                connection_destroy(conn);
                return; // Esce dalla funzione
            }
            log_debug("Ricevuti %zd byte (fd: %d)", nread, fd);
            
            // Se non stiamo usando EPOLLET, dovremmo uscire dal loop 'read' qui.
            // Ma dato che lo usiamo, continuiamo a leggere.
            
        } else if (nread == 0) {
            // Connessione chiusa dal client (EOF)
            // L'evento eof (EPOLLRDHUP/EV_EOF) gestirà questo.
            // Ma se read() restituisce 0, è comunque EOF.
            log_info("Client (fd: %d) ha chiuso (read 0)", fd);
            connection_destroy(conn);
            return; // Esce dalla funzione
            
        } else { // nread == -1
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Finito di leggere per ora (necessario per EPOLLET)
                break; // Esce dal loop 'read'
            } else {
                // Errore reale
                log_error("Errore read() su fd %d: %s. Chiudo.", fd, strerror(errno));
                connection_destroy(conn);
                return; // Esce dalla funzione
            }
        }
    } // fine while(1) di lettura

    // Dopo aver letto tutto il possibile, avviamo il parser
    if (connection_get_state(conn) != CONN_STATE_CLOSING && buffer_len(read_buf) > 0) {
        connection_set_state(conn, CONN_STATE_PARSING);
        vsp_parse_result_t parse_res = vsp_parser_execute(conn);

        if (parse_res == VSP_PARSE_ERROR) {
            log_warn("Errore protocollo (fd: %d). Chiudo.", fd);
            connection_destroy(conn);
        }
    }
}

// Funzione helper per scrivere dati a un client
static void server_handle_client_write(vex_connection_t *conn) {
    if (conn == NULL) return;

    int fd = connection_get_fd(conn);
    buffer_t *write_buf = connection_get_write_buffer(conn);
    
    // NOTA: Con EPOLLET, scriviamo finché non finiamo o dà EWOULDBLOCK
    while (buffer_len(write_buf) > 0) {
        size_t data_len = buffer_len(write_buf);
        ssize_t nwritten = write(fd, buffer_data(write_buf), data_len);

        if (nwritten > 0) {
            buffer_consume(write_buf, nwritten);
            log_debug("Scritti %zd byte (fd: %d). Rimasti %zu.", nwritten, fd, buffer_len(write_buf));
        } else if (nwritten == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Buffer kernel pieno, dobbiamo aspettare il prossimo evento
                // L'evento di scrittura (EPOLLOUT) rimane attivo
                break; // Esce dal loop 'write'
            } else {
                log_error("Errore write() su fd %d: %s. Chiudo.", fd, strerror(errno));
                connection_destroy(conn);
                return; // Esce dalla funzione
            }
        } else { // nwritten == 0 (non dovrebbe accadere)
            break;
        }
    } // fine while(buffer_len > 0)

    // Se abbiamo svuotato il buffer, smettiamo di monitorare la scrittura
    if (buffer_len(write_buf) == 0) {
        // CORREZIONE: Usa il getter e accedi al membro 'loop' del server
        vex_server_t *server = connection_get_server(conn);
        el_disable_write(server->loop, fd, (void*)conn);
    }
}


// --- Implementazione delle funzioni pubbliche ---

vex_server_t* server_create(const char *port) {
    vex_server_t *server = malloc(sizeof(vex_server_t));
    if (server == NULL) {
        log_error("malloc fallito per vex_server_t: %s", strerror(errno));
        return NULL;
    }
    memset(server->connections, 0, sizeof(server->connections));

    server->listen_fd = socket_create_and_listen(port, LISTEN_BACKLOG);
    if (server->listen_fd == -1) {
        free(server);
        return NULL;
    }

    // Crea il loop eventi astratto
    server->loop = el_create(MAX_FD); 
    if (server->loop == NULL) {
        log_fatal("Impossibile creare event loop.");
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    // Registra il socket di ascolto
    if (el_add_fd_read(server->loop, server->listen_fd, (void*)server) == -1) {
        log_fatal("Impossibile registrare listen_fd al loop.");
        el_destroy(server->loop);
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    log_info("Vex server inizializzato. In ascolto su porta %s (fd: %d)", port, server->listen_fd);
    return server;
}

void server_run(vex_server_t *server) {
    vex_event_t active_events[MAX_EVENTS];
    
    log_info("Avvio del loop eventi (Reactor)...");
    
    while (1) {
        // Chiama el_poll (astratto)
        int num_events = el_poll(server->loop, active_events, -1);

        if (num_events == -1) {
            // Errore gestito da el_poll()
            continue;
        }

        for (int i = 0; i < num_events; i++) {
            vex_event_t *e = &active_events[i];
            vex_connection_t *conn = NULL;

            // 1. Evento sul socket di ascolto
            if (e->udata == (void*)server) {
                if (e->eof || e->error) {
                    log_fatal("Socket di ascolto chiuso o in errore!");
                }
                if (e->read) {
                    server_handle_new_connection(server);
                }
            } 
            // 2. Evento su un client (udata è vex_connection_t*)
            else {
                conn = (vex_connection_t*)e->udata;
                
                // Controlla se la connessione è ancora valida
                // (potrebbe essere stata chiusa da un evento precedente nello stesso loop)
                if (conn == NULL || connection_get_state(conn) == CONN_STATE_CLOSING) {
                    continue; 
                }

                // Evento di disconnessione o errore
                if (e->eof || e->error) {
                    log_info("Client (fd: %d) disconnesso (evento EOF/ERR)", connection_get_fd(conn));
                    connection_destroy(conn);
                    continue; // Prossimo evento
                }
                
                // Evento di lettura (kqueue lo segnala anche con EOF)
                if (e->read) {
                    server_handle_client_read(conn);
                }
                
                // Evento di scrittura
                // (Controlla di nuovo lo stato, read potrebbe aver chiuso la conn)
                if (connection_get_state(conn) != CONN_STATE_CLOSING && e->write) {
                    server_handle_client_write(conn);
                }
            }
        }
    }
}

void server_destroy(vex_server_t *server) {
    if (server == NULL) return;
    log_info("Spegnimento del server...");
    
    el_del_fd(server->loop, server->listen_fd);
    close(server->listen_fd);
    
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (server->connections[fd] != NULL) {
            connection_destroy(server->connections[fd]);
        }
    }
    
    el_destroy(server->loop);
    free(server);
}

void server_remove_connection(vex_server_t *server, struct vex_connection_s *conn) {
    if (conn == NULL) return;
    int fd = connection_get_fd(conn);
    if (fd >= 0 && fd < MAX_FD) {
        if (server->connections[fd] != NULL) {
             // Rimuove l'FD dal loop prima di chiuderlo
             el_del_fd(server->loop, fd);
             server->connections[fd] = NULL;
        }
    }
}


/**
 * @brief (Fase 2) Esegue il comando VSP parsato.
 * (Logica modificata per usare la nuova astrazione el_*)
 */
void server_execute_command(struct vex_connection_s *conn, char **argv, int argc) {
    if (conn == NULL || argv == NULL || argc == 0) return;

    vex_server_t *server = connection_get_server(conn);
    buffer_t *write_buf = connection_get_write_buffer(conn);
    
    log_info("Comando (fd: %d): %s (argc: %d)", connection_get_fd(conn), argv[0], argc);

    if (strcasecmp(argv[0], "QUERY") == 0) {
        // TODO (Fase 3): Cerca in L1
        buffer_append(write_buf, "$-1\r\n", 5); // MISS (Nil)
        log_info("Risposta (fd: %d): MISS", connection_get_fd(conn));

    } else if (strcasecmp(argv[0], "SET") == 0) {
        // TODO (Fase 3): Salva in L1
        buffer_append(write_buf, "+OK\r\n", 5); // OK (Simple String)
        log_info("Risposta (fd: %d): OK", connection_get_fd(conn));
        
    } else {
        // Comando non riconosciuto
        char err_msg[100];
        snprintf(err_msg, sizeof(err_msg), "-ERR Comando sconosciuto '%s'\r\n", argv[0]);
        buffer_append(write_buf, err_msg, strlen(err_msg));
        log_warn("Risposta (fd: %d): Comando sconosciuto", connection_get_fd(conn));
    }

    // Abilita l'evento di scrittura (astratto)
    el_enable_write(server->loop, 
                    connection_get_fd(conn),
                    (void*)conn);
}