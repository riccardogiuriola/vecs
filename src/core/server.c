/*
 * Vex Project: Implementazione Server (Reactor Kqueue + Semantic Cache)
 * (src/core/server.c)
 */

#include "server.h"
#include "connection.h"
#include "logger.h"
#include "socket.h"
#include "buffer.h"
#include "vsp_parser.h"
#include "event_loop.h"
#include "hash_map.h"

// --- INTEGRAZIONE MODULI AI ---
#include "vector_engine.h"
#include "l2_cache.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h> // Per snprintf
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

// Configurazioni
#define MAX_FD 65536
#define MAX_EVENTS 64
#define VEX_BACKLOG 128
#define MAX_L1_KEY_SIZE 8192

// Soglia di similarità per L2 (0.0 - 1.0). 
#define L2_SIMILARITY_THRESHOLD 0.65f
#define L2_DEDUPE_THRESHOLD     0.98f

// Percorso del modello
#define MODEL_PATH "models/bge-m3-q4_k_m.gguf"

/**
 * @brief Struttura interna del Server
 */
struct vex_server_s {
    const char *port;
    int listen_fd;
    event_loop_t *loop;
    
    // --- CACHE LAYERS ---
    hash_map_t *l1_cache;        // L1: Exact Match (Hash Map)
    
    vector_engine_t *vec_engine; // Motore di inferenza AI
    l2_cache_t *l2_cache;        // L2: Semantic Match (Vector Store)
    
    float *tmp_vector_buf;       // Buffer riutilizzabile per i calcoli di embedding
    int vector_dim;              // Dimensione vettori

    // Gestione connessioni
    vex_connection_t *connections[MAX_FD];
    vex_event_t *events;
};

// --- Prototipi Funzioni Statiche ---
static void server_handle_client_event(vex_event_t *event);
static void server_handle_new_connection(vex_server_t *server);
static void server_handle_client_read(vex_connection_t *conn);
static void server_handle_client_write(vex_connection_t *conn);
static void server_execute_command(vex_connection_t *conn, int argc, char **argv);


// --- Gestori Eventi ---

static void server_handle_client_event(vex_event_t *event) {
    vex_connection_t *conn = (vex_connection_t*)event->udata;

    if (conn == NULL || connection_get_fd(conn) == -1) {
        return;
    }

    int fd = connection_get_fd(conn);
    
    if (event->error || event->eof) {
        if (event->error) log_warn("Errore socket (fd: %d): %s", fd, strerror(errno));
        if (event->eof) log_info("Client disconnesso (fd: %d)", fd);
        server_remove_connection(conn);
        return;
    }

    if (event->write) {
        server_handle_client_write(conn);
    }
    
    if (event->read) {
        server_handle_client_read(conn);
    }
}

static void server_handle_new_connection(vex_server_t *server) {
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd;
    
    while (1) {
        client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &addr_len);

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_error("accept() fallito: %s", strerror(errno));
            break;
        }
        
        if (client_fd >= MAX_FD) {
            log_warn("Rifiutata connessione (fd: %d): superato MAX_FD", client_fd);
            close(client_fd);
            continue;
        }

        server_add_connection(server, client_fd);
    }
}

static void server_handle_client_read(vex_connection_t *conn) {
    int fd = connection_get_fd(conn);
    buffer_t *read_buf = connection_get_read_buffer(conn);
    vsp_parser_t *parser = connection_get_parser(conn);
    ssize_t read_result;

    while (1) {
        read_result = buffer_read_from_fd(read_buf, fd);

        if (read_result == 0) {
            server_remove_connection(conn);
            return;
        } else if (read_result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_warn("Errore read() (fd: %d): %s", fd, strerror(errno));
            server_remove_connection(conn);
            return;
        }
    }

    int argc = 0;
    char **argv = NULL;
    
    while (vsp_parser_execute(parser, read_buf, &argc, &argv) == VSP_OK) {
        server_execute_command(conn, argc, argv);
        vsp_parser_free_argv(argc, argv);
        argv = NULL;
    }
    
    if (vsp_parser_get_state(parser) == VSP_STATE_ERROR) {
        log_warn("Errore protocollo (fd: %d). Chiudo.", fd);
        buffer_append_string(connection_get_write_buffer(conn), "-ERR Protocol Error\r\n");
        el_enable_write(connection_get_server(conn)->loop, fd, (void*)conn);
        connection_set_state(conn, STATE_CLOSING);
    }
}

static void server_handle_client_write(vex_connection_t *conn) {
    int fd = connection_get_fd(conn);
    buffer_t *write_buf = connection_get_write_buffer(conn);
    ssize_t write_result;

    while (buffer_len(write_buf) > 0) {
        write_result = buffer_write_to_fd(write_buf, fd);

        if (write_result <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_warn("Errore write() (fd: %d): %s", fd, strerror(errno));
            server_remove_connection(conn);
            return;
        }
    }
    
    if (buffer_len(write_buf) == 0) {
        vex_server_t *server = connection_get_server(conn);
        el_disable_write(server->loop, fd, (void*)conn);
        
        if (connection_get_state(conn) == STATE_CLOSING) {
            server_remove_connection(conn);
        }
    }
}

// --- CORE LOGIC: L1 & L2 CACHE ---

static void server_execute_command(vex_connection_t *conn, int argc, char **argv) {
    if (conn == NULL || argc == 0) return;

    vex_server_t *server = connection_get_server(conn);
    buffer_t *write_buf = connection_get_write_buffer(conn);
    hash_map_t *l1_cache = server->l1_cache;
    int fd = connection_get_fd(conn);

    char key_buf[MAX_L1_KEY_SIZE];
    char header_buf[64];

    // --- COMANDO SET ---
    if (strcasecmp(argv[0], "SET") == 0) {
        if (argc != 4) {
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'SET'\r\n");
        } else {
            // 1. Inserimento L1 (Sempre, perché L1 è exact match per chiave stringa)
            snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
            hash_map_set(l1_cache, key_buf, argv[3]);
            log_debug("SET L1 OK.");

            // 2. Inserimento L2 (Semantic - Con Deduplica)
            // Calcoliamo l'embedding
            if (vector_engine_embed(server->vec_engine, argv[1], server->tmp_vector_buf) == 0) {
                
                // STEP AGGIUNTIVO: Controlliamo se esiste già qualcosa di quasi identico
                const char *existing = l2_cache_search(
                    server->l2_cache, 
                    server->tmp_vector_buf, 
                    L2_DEDUPE_THRESHOLD // Usa soglia 0.98/0.99
                );

                if (existing != NULL) {
                    // DUPLICATO TROVATO!
                    log_info("SET L2 Skipped: Concetto semantico già presente (Score > %.2f)", L2_DEDUPE_THRESHOLD);
                } else {
                    // NUOVO CONCETTO -> INSERISCI
                    l2_cache_insert(server->l2_cache, server->tmp_vector_buf, argv[3]);
                    log_info("SET L2 OK (Nuovo concetto inserito).");
                }

            } else {
                log_error("SET L2 Fallito: Errore embedding.");
            }

            buffer_append_string(write_buf, "+OK\r\n");
        }
    
    // --- COMANDO QUERY ---
    } else if (strcasecmp(argv[0], "QUERY") == 0) {
        if (argc != 3) {
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'QUERY'\r\n");
        } else {
            // A. L1
            snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
            const char *value = hash_map_get(l1_cache, key_buf);
            
            if (value != NULL) {
                // HIT L1
                size_t val_len = strlen(value);
                snprintf(header_buf, sizeof(header_buf), "$%zu\r\n", val_len);
                buffer_append_string(write_buf, header_buf);
                buffer_append_data(write_buf, value, val_len);
                buffer_append_string(write_buf, "\r\n");
                
            } else {
                // MISS L1 -> L2
                log_info("MISS L1. Provo Semantic Search L2...");

                // Embedding della query (PURO, senza prefissi)
                if (vector_engine_embed(server->vec_engine, argv[1], server->tmp_vector_buf) == 0) {
                    
                    const char *semantic_val = l2_cache_search(
                        server->l2_cache, 
                        server->tmp_vector_buf, 
                        L2_SIMILARITY_THRESHOLD
                    );

                    if (semantic_val != NULL) {
                        // HIT L2
                        size_t val_len = strlen(semantic_val);
                        snprintf(header_buf, sizeof(header_buf), "$%zu\r\n", val_len);
                        buffer_append_string(write_buf, header_buf);
                        buffer_append_data(write_buf, semantic_val, val_len);
                        buffer_append_string(write_buf, "\r\n");
                    } else {
                        // MISS TOTALE
                        buffer_append_string(write_buf, "$-1\r\n");
                    }
                } else {
                    buffer_append_string(write_buf, "-ERR Embedding Failed\r\n");
                }
            }
        }
    } else {
        snprintf(header_buf, sizeof(header_buf), "-ERR unknown command '%s'\r\n", argv[0]);
        buffer_append_string(write_buf, header_buf);
    }
    
    el_enable_write(server->loop, fd, (void*)conn);
}


// --- Implementazione Funzioni Pubbliche ---

vex_server_t* server_create(const char *port) {
    vex_server_t *server = calloc(1, sizeof(vex_server_t));
    if (!server) return NULL;
    
    server->events = calloc(MAX_EVENTS, sizeof(vex_event_t));
    if (!server->events) {
        free(server);
        return NULL;
    }

    server->port = port;
    
    // 1. Event Loop
    server->loop = el_create(MAX_FD);
    if (!server->loop) {
        log_fatal("Impossibile creare event loop.");
        return NULL; // Leak parziale in caso di errore fatale qui, ma si esce
    }
    
    // 2. L1 Cache
    server->l1_cache = hash_map_create(1024);
    if (!server->l1_cache) {
        log_fatal("Impossibile creare L1 Cache.");
        return NULL;
    }

    // 3. AI Vector Engine (Llama.cpp)
    log_info("Caricamento modello AI da: %s ...", MODEL_PATH);
    server->vec_engine = vector_engine_init(MODEL_PATH);
    if (!server->vec_engine) {
        log_fatal("ERRORE CRITICO: Impossibile caricare il modello GGUF. Verifica che 'models/multilingual-minilm-l12-v2.q4_k_m.gguf' esista!");
        // Pulizia base e uscita
        return NULL;
    }

    // Ottieni dimensione embedding (es. 768)
    server->vector_dim = vector_engine_get_dim(server->vec_engine);
    
    // 4. L2 Cache
    server->l2_cache = l2_cache_create(server->vector_dim, 2000);
    
    // 5. Buffer temporaneo per embedding
    server->tmp_vector_buf = malloc(server->vector_dim * sizeof(float));
    if (!server->tmp_vector_buf) {
        log_fatal("OOM allocazione buffer vettoriale.");
        return NULL;
    }

    // 6. Socket Listener
    server->listen_fd = socket_create_and_listen(port, VEX_BACKLOG);
    if (server->listen_fd < 0) {
        log_fatal("Impossibile fare bind su porta %s", port);
        return NULL;
    }

    if (el_add_fd_read(server->loop, server->listen_fd, (void*)server) == -1) {
        log_fatal("Impossibile aggiungere listener al loop.");
        return NULL;
    }

    log_info("Vex Server (L1+L2 AI) avviato su porta %s. Dimensione Vettori: %d", port, server->vector_dim);
    return server;
}

void server_destroy(vex_server_t *server) {
    if (server == NULL) return;

    log_info("Arresto server...");

    for (int i = 0; i < MAX_FD; i++) {
        if (server->connections[i]) {
            server_remove_connection(server->connections[i]);
        }
    }
    
    el_del_fd(server->loop, server->listen_fd);
    close(server->listen_fd);
    
    hash_map_destroy(server->l1_cache);
    
    // Cleanup componenti AI
    vector_engine_destroy(server->vec_engine);
    l2_cache_destroy(server->l2_cache);
    free(server->tmp_vector_buf);

    el_destroy(server->loop);
    free(server->events);
    free(server);

    log_info("Server terminato.");
}

int server_run(vex_server_t *server) {
    log_info("Loop eventi in esecuzione...");

    while (1) {
        int num_events = el_poll(server->loop, server->events, -1);

        if (num_events == -1) {
            if (errno == EINTR) continue;
            log_error("Errore critico el_poll: %s", strerror(errno));
            return -1;
        }

        for (int i = 0; i < num_events; i++) {
            vex_event_t *event = &server->events[i];
            
            if (event->udata == server) {
                server_handle_new_connection(server);
            } else {
                server_handle_client_event(event);
            }
        }
    }
    return 0;
}

vex_connection_t* server_add_connection(vex_server_t *server, int client_fd) {
    if (socket_set_non_blocking(client_fd) == -1) {
        close(client_fd);
        return NULL;
    }
    
    vex_connection_t *conn = connection_create(server, client_fd);
    if (conn == NULL) {
        close(client_fd);
        return NULL;
    }
    
    server->connections[client_fd] = conn;
    
    if (el_add_fd_read(server->loop, client_fd, (void*)conn) == -1) {
        server_remove_connection(conn);
        return NULL;
    }

    log_info("Client connesso (fd: %d)", client_fd);
    return conn;
}

void server_remove_connection(vex_connection_t *conn) {
    if (conn == NULL) return;
    
    vex_server_t *server = connection_get_server(conn);
    int fd = connection_get_fd(conn);

    if (fd == -1) return;

    el_del_fd(server->loop, fd);
    
    if (server->connections[fd] == conn) {
        server->connections[fd] = NULL;
    }
    
    connection_destroy(conn);
}

event_loop_t* server_get_loop(vex_server_t *server) {
    return server->loop;
}

hash_map_t* server_get_l1_cache(vex_server_t *server) {
    return server->l1_cache;
}