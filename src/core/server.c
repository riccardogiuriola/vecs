/*
 * Vex Project: Implementazione Server (Reactor Kqueue)
 * (src/core/server.c)
 *
 * --- VERSIONE COMPLETA (Post-Fase 3 & Fase 6) ---
 */

#include "server.h"
#include "connection.h" // Include le definizioni per la gestione connessione
#include "logger.h"
#include "socket.h"       // Per socket_set_non_blocking, socket_create_and_listen
#include "buffer.h"       // Per buffer_read_from_fd, buffer_append_string, etc.
#include "vsp_parser.h"   // Per il parser VSP
#include "event_loop.h"   // Per l'API di astrazione (el_poll, etc.)
#include "hash_map.h"     // Per la cache L1

#include <stdlib.h>
#include <string.h>
#include <stdio.h> // Per snprintf
#include <errno.h>
#include <unistd.h>     // Per close()
#include <sys/socket.h> // Per sockaddr_storage, socklen_t

// Limite massimo di file descriptor (connessioni)
// (Questo è un limite "fisso" per il nostro array;
// il limite reale è imposto da ulimit)
#define MAX_FD 65536
// Quanti eventi el_poll può restituire in un colpo solo
#define MAX_EVENTS 64
// Backlog per listen()
#define VEX_BACKLOG 128
// Dimensione massima per la chiave L1 (prompt + params)
#define MAX_L1_KEY_SIZE 8192

/**
 * @brief Struttura interna (definizione dell'handle opaco)
 * Contiene lo stato globale del server.
 */
struct vex_server_s {
    const char *port;
    int listen_fd;
    event_loop_t *loop; // L'event loop (kqueue/epoll)
    hash_map_t *l1_cache; // La cache L1

    // Array (indicizzato per FD) per accesso O(1) alle connessioni
    vex_connection_t *connections[MAX_FD];

    // Array pre-allocato per gli eventi
    vex_event_t *events;
};

// --- Prototipi Funzioni Statiche (private) ---

/**
 * @brief Gestisce un evento su un FD client (lettura, scrittura, errore).
 */
static void server_handle_client_event(vex_event_t *event);

/**
 * @brief Gestisce una nuova connessione in entrata sul listener FD.
 */
static void server_handle_new_connection(vex_server_t *server);

/**
 * @brief Chiamato quando un client è leggibile (ci sono dati).
 */
static void server_handle_client_read(vex_connection_t *conn);

/**
 * @brief Chiamato quando un client è scrivibile (buffer pronto).
 */
static void server_handle_client_write(vex_connection_t *conn);

/**
 * @brief Esegue un comando VSP (SET/QUERY) dopo che il parser ha avuto successo.
 */
static void server_execute_command(vex_connection_t *conn, int argc, char **argv);


// --- Gestori Eventi ---

static void server_handle_client_event(vex_event_t *event) {
    // Estrae la connessione dall'evento
    vex_connection_t *conn = (vex_connection_t*)event->udata;

    // Controllo di sicurezza: la connessione potrebbe essere stata chiusa
    // da un evento precedente nello stesso batch di el_poll
    if (conn == NULL || connection_get_fd(conn) == -1) {
        log_debug("Evento ignorato per FD già chiuso.");
        return;
    }

    int fd = connection_get_fd(conn);
    
    // Gestione Errore o Disconnessione (priorità)
    if (event->error || event->eof) {
        if (event->error) {
            log_warn("Errore socket (fd: %d): %s", fd, strerror(errno));
        }
        if (event->eof) {
            log_info("Client disconnesso (fd: %d)", fd);
        }
        server_remove_connection(conn);
        return;
    }

    // Gestione Scrittura (se pronto a scrivere)
    if (event->write) {
        // log_debug("Evento SCRITTURA (fd: %d)", fd);
        server_handle_client_write(conn);
    }
    
    // Gestione Lettura (se ci sono dati)
    // (Potrebbe essere eseguito dopo la scrittura se entrambi gli eventi
    // sono stati attivati, es. read + eof)
    if (event->read) {
        // log_debug("Evento LETTURA (fd: %d)", fd);
        server_handle_client_read(conn);
    }
}


static void server_handle_new_connection(vex_server_t *server) {
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd;
    
    // Cicla (perché il listener è Edge-Triggered)
    while (1) {
        client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &addr_len);

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Finiti i client da accettare
                break;
            } else {
                log_error("accept() fallito: %s", strerror(errno));
                break;
            }
        }
        
        if (client_fd >= MAX_FD) {
            log_warn("Rifiutata connessione (fd: %d): superato MAX_FD (%d)", client_fd, MAX_FD);
            close(client_fd);
            continue;
        }

        // Aggiunge la connessione al server
        server_add_connection(server, client_fd);
    }
}


static void server_handle_client_read(vex_connection_t *conn) {
    int fd = connection_get_fd(conn);
    buffer_t *read_buf = connection_get_read_buffer(conn);
    vsp_parser_t *parser = connection_get_parser(conn);
    ssize_t read_result;

    // Loop di lettura (per Edge-Triggered: svuota il buffer del kernel)
    while (1) {
        read_result = buffer_read_from_fd(read_buf, fd);

        if (read_result == 0) {
            // EOF - Client ha chiuso la connessione
            log_info("Client disconnesso (EOF) (fd: %d)", fd);
            server_remove_connection(conn);
            return;
        
        } else if (read_result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Finito di leggere per ora (EAGAIN)
                break; 
            } else {
                // Errore reale
                log_warn("Errore read() (fd: %d): %s", fd, strerror(errno));
                server_remove_connection(conn);
                return;
            }
        
        } else {
            // Dati letti con successo (read_result > 0)
            log_debug("Ricevuti %zd byte (fd: %d)", read_result, fd);
        }
    } // Fine loop di lettura (ET)

    // Ora che abbiamo letto tutto il possibile, esegui il parser
    // Esegui il parser in un loop, potrebbe esserci più di un comando
    // nel buffer (pipelining)
    int argc = 0;
    char **argv = NULL;
    
    while (vsp_parser_execute(parser, read_buf, &argc, &argv) == VSP_OK) {
        // Abbiamo un comando completo!
        server_execute_command(conn, argc, argv);
        vsp_parser_free_argv(argc, argv);
        argv = NULL; // Resetta per il prossimo loop
    }
    
    // Controlla se il parser è in uno stato di errore
    if (vsp_parser_get_state(parser) == VSP_STATE_ERROR) {
        log_warn("Errore protocollo (fd: %d). Chiudo.", fd);
        // Prepara una risposta di errore e chiudi
        buffer_append_string(connection_get_write_buffer(conn), "-ERR Protocol Error\r\n");
        el_enable_write(connection_get_server(conn)->loop, fd, (void*)conn);
        connection_set_state(conn, STATE_CLOSING);
    }
}


static void server_handle_client_write(vex_connection_t *conn) {
    int fd = connection_get_fd(conn);
    buffer_t *write_buf = connection_get_write_buffer(conn);
    ssize_t write_result;

    // Loop di scrittura (per Edge-Triggered: svuota il nostro buffer)
    while (buffer_len(write_buf) > 0) {
        write_result = buffer_write_to_fd(write_buf, fd);

        if (write_result <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Il buffer di scrittura del kernel è pieno (EAGAIN)
                // Dobbiamo aspettare il prossimo evento "write ready"
                break; 
            } else {
                // Errore reale
                log_warn("Errore write() (fd: %d): %s", fd, strerror(errno));
                server_remove_connection(conn);
                return;
            }
        } else {
            // Dati scritti con successo
            log_debug("Scritti %zd byte (fd: %d)", write_result, fd);
        }
    } // Fine loop di scrittura (ET)
    
    // Se abbiamo svuotato il buffer, smetti di monitorare la scrittura
    if (buffer_len(write_buf) == 0) {
        vex_server_t *server = connection_get_server(conn);
        el_disable_write(server->loop, fd, (void*)conn);
        
        // Se eravamo in attesa di chiudere, chiudi ora
        if (connection_get_state(conn) == STATE_CLOSING) {
            log_debug("Buffer svuotato, chiudo (fd: %d)", fd);
            server_remove_connection(conn);
        }
    }
}


static void server_execute_command(vex_connection_t *conn, int argc, char **argv) {
    if (conn == NULL || argc == 0) return;

    vex_server_t *server = connection_get_server(conn);
    buffer_t *write_buf = connection_get_write_buffer(conn);
    hash_map_t *cache = server->l1_cache;
    int fd = connection_get_fd(conn);

    log_debug("Comando VSP eseguito (fd: %d): %s", fd, argv[0]);

    // Buffer per la chiave L1 (prompt + params)
    char key_buf[MAX_L1_KEY_SIZE];
    // Buffer per l'header della risposta (es. "$12\r\n")
    char header_buf[32];


    // Comando SET: *4 $3 SET $prompt $params $response
    // argv[0] = "SET"
    // argv[1] = prompt
    // argv[2] = params (JSON string)
    // argv[3] = response
    if (strcasecmp(argv[0], "SET") == 0) {
        if (argc != 4) {
            log_warn("SET L1 (fd: %d) Argomenti errati (ricevuti %d, attesi 4)", fd, argc);
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'SET'\r\n");
        } else {
            // Costruisci la chiave L1: "prompt|params"
            snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
            
            if (hash_map_set(cache, key_buf, argv[3]) == 0) {
                log_debug("SET L1 (fd: %d) Chiave: '%.50s...'", fd, key_buf);
                buffer_append_string(write_buf, "+OK\r\n");
            } else {
                log_warn("SET L1 (fd: %d) Fallita allocazione memoria (OOM)", fd);
                buffer_append_string(write_buf, "-ERR out of memory\r\n");
            }
        }
    
    // Comando QUERY: *3 $5 QUERY $prompt $params
    // argv[0] = "QUERY"
    // argv[1] = prompt
    // argv[2] = params (JSON string)
    } else if (strcasecmp(argv[0], "QUERY") == 0) {
        if (argc != 3) {
            log_warn("QUERY L1 (fd: %d) Argomenti errati (ricevuti %d, attesi 3)", fd, argc);
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'QUERY'\r\n");
        } else {
            // Costruisci la chiave L1: "prompt|params"
            snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
            
            const char *value = hash_map_get(cache, key_buf);
            
            if (value != NULL) {
                // --- HIT L1 ---
                log_debug("HIT L1 (fd: %d)", fd);
                size_t value_len = strlen(value);
                
                // Formatta la risposta VSP (Bulk String)
                // 1. Header (es. "$123\r\n")
                snprintf(header_buf, sizeof(header_buf), "$%zu\r\n", value_len);
                buffer_append_string(write_buf, header_buf);
                
                // 2. Dati
                buffer_append_data(write_buf, value, value_len);
                
                // 3. CRLF finale
                buffer_append_string(write_buf, "\r\n");
                
            } else {
                // --- MISS L1 ---
                log_debug("MISS L1 (fd: %d)", fd);
                // Fase 4/5 (Cache L2) andrà qui
                
                // Per ora (Fase 3), rispondiamo VSP Null
                buffer_append_string(write_buf, "$-1\r\n");
            }
        }
    
    } else {
        // Comando non riconosciuto
        log_warn("Comando non riconosciuto (fd: %d): %s", fd, argv[0]);
        snprintf(header_buf, sizeof(header_buf), "-ERR unknown command '%s'\r\n", argv[0]);
        buffer_append_string(write_buf, header_buf);
    }
    
    // Abilita la scrittura per inviare la risposta
    el_enable_write(server->loop, fd, (void*)conn);
}


// --- Implementazione delle funzioni pubbliche ---

vex_server_t* server_create(const char *port) {
    // calloc inizializza a zero, quindi connections[] è pieno di NULL
    vex_server_t *server = calloc(1, sizeof(vex_server_t));
    if (server == NULL) {
        log_fatal("server_create: Impossibile allocare memoria per il server: %s", strerror(errno));
        return NULL;
    }
    
    // Alloca l'array per gli eventi
    server->events = calloc(MAX_EVENTS, sizeof(vex_event_t));
    if (server->events == NULL) {
        log_fatal("server_create: Impossibile allocare memoria per gli eventi: %s", strerror(errno));
        free(server);
        return NULL;
    }

    server->port = port;
    
    // Crea l'event loop (kqueue o epoll)
    server->loop = el_create(MAX_FD);
    if (server->loop == NULL) {
        log_fatal("server_create: Impossibile creare l'event loop.");
        free(server->events);
        free(server);
        return NULL;
    }
    
    // Crea la cache L1
    server->l1_cache = hash_map_create(1024); // Capacità iniziale di 1024
    if (server->l1_cache == NULL) {
        log_fatal("server_create: Impossibile creare la cache L1.");
        el_destroy(server->loop);
        free(server->events);
        free(server);
        return NULL;
    }

    // Apri il socket di ascolto
    server->listen_fd = socket_create_and_listen(port, VEX_BACKLOG);
    if (server->listen_fd < 0) {
        log_fatal("server_create: Impossibile creare il socket listener sulla porta %s", port);
        hash_map_destroy(server->l1_cache);
        el_destroy(server->loop);
        free(server->events);
        free(server);
        return NULL;
    }

    // Registra l'evento per nuove connessioni
    // Usiamo 'server' come udata per identificare il listener FD
    if (el_add_fd_read(server->loop, server->listen_fd, (void*)server) == -1) {
        log_fatal("server_create: Impossibile aggiungere listener FD all'event loop.");
        close(server->listen_fd);
        hash_map_destroy(server->l1_cache);
        el_destroy(server->loop);
        free(server->events);
        free(server);
        return NULL;
    }

    log_info("Vex server inizializzato. In ascolto su porta %s (fd: %d)",
             server->port, server->listen_fd);

    return server;
}


void server_destroy(vex_server_t *server) {
    if (server == NULL) return;

    log_info("Chiusura del server...");

    // Chiudi tutte le connessioni client attive
    for (int i = 0; i < MAX_FD; i++) {
        if (server->connections[i]) {
            // server_remove_connection si occupa di el_del_fd e connection_destroy
            server_remove_connection(server->connections[i]);
        }
    }
    
    // Rimuovi il listener (anche se close() lo farebbe implicitamente)
    el_del_fd(server->loop, server->listen_fd);
    close(server->listen_fd);
    
    // Distruggi i componenti principali
    hash_map_destroy(server->l1_cache);
    el_destroy(server->loop);
    free(server->events);
    free(server);

    log_info("Server arrestato con successo.");
}


int server_run(vex_server_t *server) {
    log_info("Avvio del loop eventi (Reactor)...");

    while (1) {
        // Attendi gli eventi (-1 = attendi indefinitamente)
        int num_events = el_poll(server->loop, server->events, -1);

        if (num_events == -1) {
            // Errore critico nel loop
            if (errno == EINTR) continue; // Segnale, riprova
            log_error("Errore critico in el_poll: %s", strerror(errno));
            return -1; // Esce dal loop
        }

        // Processa tutti gli eventi
        for (int i = 0; i < num_events; i++) {
            vex_event_t *event = &server->events[i];
            
            // Distingui tra nuova connessione (udata == server)
            // e connessione client (udata == conn)
            if (event->udata == server) {
                // Evento sul listener FD
                server_handle_new_connection(server);
            } else {
                // Evento su un client FD
                server_handle_client_event(event);
            }
        }
    }

    return 0;
}


// --- Funzioni di Gestione Connessione ---

vex_connection_t* server_add_connection(vex_server_t *server, int client_fd) {
    // Imposta il socket come non-bloccante
    if (socket_set_non_blocking(client_fd) == -1) {
        log_warn("Impossibile impostare non-bloccante (fd: %d)", client_fd);
        close(client_fd);
        return NULL;
    }
    
    // Crea la struttura della connessione
    vex_connection_t *conn = connection_create(server, client_fd);
    if (conn == NULL) {
        log_warn("Fallita creazione struttura connessione (fd: %d)", client_fd);
        close(client_fd);
        return NULL;
    }
    
    // Aggiungi al pool
    server->connections[client_fd] = conn;
    
    // Aggiungi all'event loop (per LETTURA)
    if (el_add_fd_read(server->loop, client_fd, (void*)conn) == -1) {
        log_warn("Impossibile aggiungere client a event loop (fd: %d)", client_fd);
        server_remove_connection(conn); // Esegue il cleanup
        return NULL;
    }

    log_info("Nuovo client connesso (fd: %d)", client_fd);
    return conn;
}


void server_remove_connection(vex_connection_t *conn) {
    if (conn == NULL) return;
    
    vex_server_t *server = connection_get_server(conn);
    int fd = connection_get_fd(conn);

    if (fd == -1) {
        // Già rimosso (prevenzione doppia chiamata)
        return;
    }

    log_debug("Rimozione connessione (fd: %d)", fd);
    
    // Rimuovi dall'event loop
    el_del_fd(server->loop, fd);
    
    // Rimuovi dal pool
    if (server->connections[fd] == conn) {
        server->connections[fd] = NULL;
    }
    
    // Distruggi la struttura (chiude FD, libera buffer, etc.)
    connection_destroy(conn);
}


// --- Getters ---

event_loop_t* server_get_loop(vex_server_t *server) {
    return server->loop;
}

hash_map_t* server_get_l1_cache(vex_server_t *server) {
    return server->l1_cache;
}