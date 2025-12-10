/*
 * Vecs Project: Implementazione Server (Reactor Kqueue + Semantic Cache)
 * (src/core/server.c)
 * * Includes: Env Configuration, L1 Cache, L2 Cache (Vector Search)
 */

#include "server.h"
#include "connection.h"
#include "logger.h"
#include "socket.h"
#include "buffer.h"
#include "vsp_parser.h"
#include "event_loop.h"
#include "hash_map.h"
#include "vector_engine.h"
#include "l2_cache.h"
#include "text.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h> // Per snprintf
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

// Configurazioni Statiche
#define MAX_FD 65536
#define MAX_EVENTS 64
#define VECS_BACKLOG 128
#define MAX_L1_KEY_SIZE 8192

// --- DEFAULTS (Fallback se ENV non settate) ---
// Usiamo BGE-M3 come default robusto
#define DEFAULT_MODEL_PATH "models/default_model.gguf"
// Soglia conservativa per BGE (0.65 è ottimo per Q4_K_M)
#define DEFAULT_L2_THRESHOLD "0.65"
// Soglia alta per evitare duplicati quasi identici
#define DEFAULT_L2_DEDUPE "0.95"
// Capacità vettoriale di default
#define DEFAULT_L2_CAPACITY "5000"

/**
 * @brief Configurazione Runtime (caricata da ENV)
 */
typedef struct {
    char model_path[512];
    float l2_threshold;
    float l2_dedupe_threshold;
    int l2_capacity;
} vecs_config_t;

/**
 * @brief Struttura interna del Server
 */
struct vecs_server_s {
    const char *port;
    int listen_fd;
    event_loop_t *loop;
    
    // Configurazione Dinamica
    vecs_config_t config;

    // --- CACHE LAYERS ---
    hash_map_t *l1_cache;        // L1: Exact Match
    vector_engine_t *vec_engine; // AI Engine
    l2_cache_t *l2_cache;        // L2: Semantic Match
    
    float *tmp_vector_buf;       // Buffer per embedding
    int vector_dim;              // Dimensione vettori (letto dal modello)

    // Gestione connessioni
    vecs_connection_t *connections[MAX_FD];
    vecs_event_t *events;
};

// --- Helpers per ENV ---

static const char* get_env_string(const char* key, const char* default_val) {
    const char* val = getenv(key);
    return val ? val : default_val;
}

static float get_env_float(const char* key, const char* default_val) {
    const char* val = getenv(key);
    if (!val) val = default_val;
    return strtof(val, NULL);
}

static int get_env_int(const char* key, const char* default_val) {
    const char* val = getenv(key);
    if (!val) val = default_val;
    return atoi(val);
}

// --- Prototipi Funzioni Statiche ---
static void server_handle_client_event(vecs_event_t *event);
static void server_handle_new_connection(vecs_server_t *server);
static void server_handle_client_read(vecs_connection_t *conn);
static void server_handle_client_write(vecs_connection_t *conn);
static void server_execute_command(vecs_connection_t *conn, int argc, char **argv);
void server_remove_connection(vecs_connection_t *conn);


// --- Gestori Eventi (Network) ---

static void server_handle_client_event(vecs_event_t *event) {
    vecs_connection_t *conn = (vecs_connection_t*)event->udata;

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

static void server_handle_new_connection(vecs_server_t *server) {
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

static void server_handle_client_read(vecs_connection_t *conn) {
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

static void server_handle_client_write(vecs_connection_t *conn) {
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
        vecs_server_t *server = connection_get_server(conn);
        el_disable_write(server->loop, fd, (void*)conn);
        
        if (connection_get_state(conn) == STATE_CLOSING) {
            server_remove_connection(conn);
        }
    }
}

// --- CORE LOGIC: L1 & L2 CACHE (Configurable) ---

static void server_execute_command(vecs_connection_t *conn, int argc, char **argv) {
    if (conn == NULL || argc == 0) return;

    vecs_server_t *server = connection_get_server(conn);
    buffer_t *write_buf = connection_get_write_buffer(conn);
    hash_map_t *l1_cache = server->l1_cache;
    int fd = connection_get_fd(conn);

    char key_buf[MAX_L1_KEY_SIZE];
    char header_buf[64];

    // --- COMANDO SET ---
    // *4 $3 SET $prompt $params $response
    if (strcasecmp(argv[0], "SET") == 0) {
        if (argc != 4) {
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'SET'\r\n");
        } else {
            // 1. Inserimento L1 (Sempre, perché L1 è exact match per chiave stringa)
            snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
            hash_map_set(l1_cache, key_buf, argv[3]);
            log_debug("SET L1 OK.");

            // 2. Inserimento L2 (Semantic - Con Deduplica Dinamica)
            // Calcoliamo l'embedding del prompt
            char clean_prompt[4096];
            normalize_text(argv[1], clean_prompt, sizeof(clean_prompt));
            if (vector_engine_embed(server->vec_engine, clean_prompt, server->tmp_vector_buf) == 0) {
                
                // STEP DEDUPLICAZIONE:
                // Usiamo la soglia configurata (es. 0.95) per vedere se esiste già
                const char *existing = l2_cache_search(
                    server->l2_cache, 
                    server->tmp_vector_buf, 
                    argv[1],
                    server->config.l2_dedupe_threshold
                );

                if (existing != NULL) {
                    // DUPLICATO TROVATO!
                    log_info("SET L2 Skipped: Concetto semantico già presente (Score > %.2f)", server->config.l2_dedupe_threshold);
                } else {
                    // NUOVO CONCETTO -> INSERISCI
                    l2_cache_insert(server->l2_cache, server->tmp_vector_buf, argv[1], argv[3]);
                    log_info("SET L2 OK (Nuovo concetto inserito).");
                }

            } else {
                log_error("SET L2 Fallito: Errore embedding.");
            }

            buffer_append_string(write_buf, "+OK\r\n");
        }
    
    // --- COMANDO QUERY ---
    // *3 $5 QUERY $prompt $params
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
                log_debug("MISS L1. Provo Semantic Search L2...");

                // Embedding della query (PURO, senza prefissi per BGE-M3/MiniLM)
                char clean_prompt[4096];
                normalize_text(argv[1], clean_prompt, sizeof(clean_prompt));
                if (vector_engine_embed(server->vec_engine, clean_prompt, server->tmp_vector_buf) == 0) {
                    
                    const char *semantic_val = l2_cache_search(
                        server->l2_cache, 
                        server->tmp_vector_buf,
                        argv[1],
                        server->config.l2_threshold
                    );

                    if (semantic_val != NULL) {
                        // HIT L2
                        log_info("HIT L2 (Semantic Match)!");
                        
                        size_t val_len = strlen(semantic_val);
                        snprintf(header_buf, sizeof(header_buf), "$%zu\r\n", val_len);
                        buffer_append_string(write_buf, header_buf);
                        buffer_append_data(write_buf, semantic_val, val_len);
                        buffer_append_string(write_buf, "\r\n");
                    } else {
                        // MISS TOTALE
                        log_debug("MISS L2 (Nessuna similarità > %.2f)", server->config.l2_threshold);
                        buffer_append_string(write_buf, "$-1\r\n");
                    }
                } else {
                    buffer_append_string(write_buf, "-ERR Embedding Failed\r\n");
                }
            }
        }
    } else if (strcasecmp(argv[0], "DELETE") == 0) {
        if (argc != 3) {
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'DELETE'\r\n");
        } else {
            int deleted_count = 0;

            // 1. Cancella da L1 (Exact Match)
            snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
            
            // Nota: hash_map_delete è void, ma assumiamo cancelli se esiste.
            // Se volessimo contare, dovremmo modificare hash_map_delete per ritornare int.
            // Per ora procediamo.
            hash_map_delete(l1_cache, key_buf);
            // Diciamo che L1 conta come 1 se c'era (non lo sappiamo con l'API attuale, ma fa nulla)
            
            // 2. Cancella da L2 (Semantic Match)
            // Calcoliamo l'embedding del prompt da cancellare
            char clean_prompt[4096];
            normalize_text(argv[1], clean_prompt, sizeof(clean_prompt));
            if (vector_engine_embed(server->vec_engine, clean_prompt, server->tmp_vector_buf) == 0) {
                if (l2_cache_delete_semantic(server->l2_cache, server->tmp_vector_buf)) {
                    deleted_count++;
                }
            }

            // Rispondiamo con un Intero (formato RESP ":<num>\r\n") 
            // oppure OK per semplicità. Usiamo OK per coerenza col client attuale.
            buffer_append_string(write_buf, "+OK\r\n");
            
            log_info("DELETE eseguito per '%s' (L2 rimossi: %d)", argv[1], deleted_count);
        }
    } else {
        snprintf(header_buf, sizeof(header_buf), "-ERR unknown command '%s'\r\n", argv[0]);
        buffer_append_string(write_buf, header_buf);
    }
    
    el_enable_write(server->loop, fd, (void*)conn);
}


// --- Init & Lifecycle ---

vecs_server_t* server_create(const char *port) {
    vecs_server_t *server = calloc(1, sizeof(vecs_server_t));
    if (!server) return NULL;
    
    server->events = calloc(MAX_EVENTS, sizeof(vecs_event_t));
    if (!server->events) {
        free(server);
        return NULL;
    }

    server->port = port;

    // --- CARICAMENTO CONFIGURAZIONE DA ENV ---
    strncpy(server->config.model_path, get_env_string("VECS_MODEL_PATH", DEFAULT_MODEL_PATH), 511);
    server->config.l2_threshold = get_env_float("VECS_L2_THRESHOLD", DEFAULT_L2_THRESHOLD);
    server->config.l2_dedupe_threshold = get_env_float("VECS_L2_DEDUPE_THRESHOLD", DEFAULT_L2_DEDUPE);
    server->config.l2_capacity = get_env_int("VECS_L2_CAPACITY", DEFAULT_L2_CAPACITY);

    log_info("=== VECS CONFIG ===");
    log_info("Model Path:   %s", server->config.model_path);
    log_info("L2 Threshold: %.2f", server->config.l2_threshold);
    log_info("L2 Dedupe:    %.2f", server->config.l2_dedupe_threshold);
    log_info("L2 Capacity:  %d vectors", server->config.l2_capacity);
    log_info("==================");
    
    // 1. Event Loop
    server->loop = el_create(MAX_FD);
    if (!server->loop) {
        log_fatal("Impossibile creare event loop.");
        return NULL; 
    }
    
    // 2. L1 Cache
    server->l1_cache = hash_map_create(1024);
    if (!server->l1_cache) {
        log_fatal("Impossibile creare L1 Cache.");
        return NULL;
    }

    // 3. AI Vector Engine
    log_info("Caricamento modello AI...");
    server->vec_engine = vector_engine_init(server->config.model_path);
    if (!server->vec_engine) {
        log_fatal("ERRORE CRITICO: Impossibile caricare il modello GGUF da '%s'.", server->config.model_path);
        // Clean up parziale...
        return NULL;
    }

    // Ottieni dimensione embedding (dinamica, letta dal modello)
    server->vector_dim = vector_engine_get_dim(server->vec_engine);
    
    // 4. L2 Cache
    server->l2_cache = l2_cache_create(server->vector_dim, server->config.l2_capacity);
    
    // 5. Buffer temporaneo per embedding
    server->tmp_vector_buf = malloc(server->vector_dim * sizeof(float));
    if (!server->tmp_vector_buf) {
        log_fatal("OOM allocazione buffer vettoriale.");
        return NULL;
    }

    // 6. Socket Listener
    server->listen_fd = socket_create_and_listen(port, VECS_BACKLOG);
    if (server->listen_fd < 0) {
        log_fatal("Impossibile fare bind su porta %s", port);
        return NULL;
    }

    if (el_add_fd_read(server->loop, server->listen_fd, (void*)server) == -1) {
        log_fatal("Impossibile aggiungere listener al loop.");
        return NULL;
    }

    log_info("Vecs Server avviato. Listening :%s. Vector Dim: %d", port, server->vector_dim);
    return server;
}

void server_destroy(vecs_server_t *server) {
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

int server_run(vecs_server_t *server) {
    log_info("Loop eventi in esecuzione...");

    while (1) {
        int num_events = el_poll(server->loop, server->events, -1);

        if (num_events == -1) {
            if (errno == EINTR) continue;
            log_error("Errore critico el_poll: %s", strerror(errno));
            return -1;
        }

        for (int i = 0; i < num_events; i++) {
            vecs_event_t *event = &server->events[i];
            
            if (event->udata == server) {
                server_handle_new_connection(server);
            } else {
                server_handle_client_event(event);
            }
        }
    }
    return 0;
}

vecs_connection_t* server_add_connection(vecs_server_t *server, int client_fd) {
    if (socket_set_non_blocking(client_fd) == -1) {
        close(client_fd);
        return NULL;
    }
    
    vecs_connection_t *conn = connection_create(server, client_fd);
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

void server_remove_connection(vecs_connection_t *conn) {
    if (conn == NULL) return;
    
    vecs_server_t *server = connection_get_server(conn);
    int fd = connection_get_fd(conn);

    if (fd == -1) return;

    el_del_fd(server->loop, fd);
    
    if (server->connections[fd] == conn) {
        server->connections[fd] = NULL;
    }
    
    connection_destroy(conn);
}

event_loop_t* server_get_loop(vecs_server_t *server) {
    return server->loop;
}

hash_map_t* server_get_l1_cache(vecs_server_t *server) {
    return server->l1_cache;
}