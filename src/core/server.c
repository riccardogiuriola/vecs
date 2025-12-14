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
#include "worker_pool.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h> // Per snprintf
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

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
#define DEFAULT_TTL "3600"
#define DEFAULT_SAVE_INTERVAL "300"
#define DUMP_DIR "data"
#define DUMP_FILENAME "data/dump.vecs"

/**
 * @brief Configurazione Runtime (caricata da ENV)
 */
typedef struct {
    char model_path[512];
    float l2_threshold;
    float l2_dedupe_threshold;
    int l2_capacity;
    int default_ttl;
    int save_interval_seconds;
    int num_workers;
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

    time_t last_save_time;
    worker_pool_t *worker_pool;

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
static void server_save_data(vecs_server_t *server);
static void server_load_data(vecs_server_t *server);
static void server_handle_worker_notification(vecs_server_t *server);

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
    uint64_t conn_id = connection_get_id(conn); // Necessario per sicurezza asincrona

    char key_buf[MAX_L1_KEY_SIZE];
    char header_buf[64];
    char clean_prompt[4096]; // Buffer per normalizzazione testo

    // --- COMANDO SET ---
    // Sintassi: SET <prompt> <params> <response> [ttl]
    if (strcasecmp(argv[0], "SET") == 0) {
        if (argc < 4 || argc > 5) {
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'SET'\r\n");
            el_enable_write(server->loop, fd, (void*)conn);
            return;
        }

        // 0. Determina TTL
        int ttl = server->config.default_ttl;
        if (argc == 5) {
            ttl = atoi(argv[4]);
            if (ttl <= 0) ttl = server->config.default_ttl;
        }

        // 1. Inserimento L1 (Sincrono, è velocissimo O(1))
        snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
        hash_map_set(l1_cache, key_buf, argv[3], ttl);
        log_debug("SET L1 OK (Sync). Preparing Async L2...");

        // 2. Inserimento L2 (ASINCRONO)
        // Normalizziamo qui nel main thread (operazione leggera string-based)
        normalize_text(argv[1], clean_prompt, sizeof(clean_prompt));

        // Creiamo il Job
        bg_job_t *job = calloc(1, sizeof(bg_job_t));
        if (!job) {
            log_error("OOM creating SET job");
            buffer_append_string(write_buf, "-ERR Server Out of Memory\r\n");
            el_enable_write(server->loop, fd, (void*)conn);
            return;
        }

        job->type = JOB_SET;
        job->client_fd = fd;
        job->conn_id = conn_id;
        job->ttl = ttl;
        
        // Copiamo i dati perché argv verrà distrutto al ritorno della funzione
        job->text_to_embed = strdup(clean_prompt); // Testo pulito per embedding
        job->key_part_1 = strdup(argv[1]);         // Prompt originale
        job->value = strdup(argv[3]);              // Risposta da salvare
        
        // Inviamo al pool
        if (wp_submit(server->worker_pool, job) != 0) {
            buffer_append_string(write_buf, "-ERR Job Queue Full\r\n");
            // Free manuale se submit fallisce
            free(job->text_to_embed); free(job->key_part_1); free(job->value); free(job);
            el_enable_write(server->loop, fd, (void*)conn);
        }

        // NOTA: NON inviamo "+OK" qui! 
        // Lo farà server_handle_worker_notification quando il worker avrà finito.
        return; 
    }

    // --- COMANDO QUERY ---
    // Sintassi: QUERY <prompt> <params>
    else if (strcasecmp(argv[0], "QUERY") == 0) {
        if (argc != 3) {
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'QUERY'\r\n");
            el_enable_write(server->loop, fd, (void*)conn);
            return;
        }

        // A. Cerca in L1 (Sincrono)
        snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
        const char *value = hash_map_get(l1_cache, key_buf);
        
        if (value != NULL) {
            // HIT L1: Rispondiamo subito!
            size_t val_len = strlen(value);
            snprintf(header_buf, sizeof(header_buf), "$%zu\r\n", val_len);
            buffer_append_string(write_buf, header_buf);
            buffer_append_data(write_buf, value, val_len);
            buffer_append_string(write_buf, "\r\n");
            
            // Abilita scrittura e chiudi
            el_enable_write(server->loop, fd, (void*)conn);
            return;
        }

        // B. MISS L1 -> Cerca in L2 (ASINCRONO)
        log_debug("MISS L1. Scheduling Async L2 Search...");

        normalize_text(argv[1], clean_prompt, sizeof(clean_prompt));

        bg_job_t *job = calloc(1, sizeof(bg_job_t));
        if (!job) {
            buffer_append_string(write_buf, "-ERR Server OOM\r\n");
            el_enable_write(server->loop, fd, (void*)conn);
            return;
        }

        job->type = JOB_QUERY;
        job->client_fd = fd;
        job->conn_id = conn_id;
        
        job->text_to_embed = strdup(clean_prompt);
        job->key_part_1 = strdup(argv[1]); // Serve per i filtri semantici dopo

        if (wp_submit(server->worker_pool, job) != 0) {
            buffer_append_string(write_buf, "-ERR Job Queue Full\r\n");
            free(job->text_to_embed); free(job->key_part_1); free(job);
            el_enable_write(server->loop, fd, (void*)conn);
        }

        // NON rispondiamo ancora. Attendiamo il worker.
        return;
    }

    // --- COMANDO DELETE ---
    // Sintassi: DELETE <prompt> <params>
    else if (strcasecmp(argv[0], "DELETE") == 0) {
        if (argc != 3) {
            buffer_append_string(write_buf, "-ERR wrong number of arguments for 'DELETE'\r\n");
            el_enable_write(server->loop, fd, (void*)conn);
            return;
        }

        // 1. Cancella da L1 (Sincrono)
        snprintf(key_buf, MAX_L1_KEY_SIZE, "%s|%s", argv[1], argv[2]);
        hash_map_delete(l1_cache, key_buf);

        // 2. Cancella da L2 (ASINCRONO)
        // Anche se DELETE è rara, calcolare l'embedding per trovarlo è lento.
        normalize_text(argv[1], clean_prompt, sizeof(clean_prompt));

        bg_job_t *job = calloc(1, sizeof(bg_job_t));
        job->type = JOB_DELETE;
        job->client_fd = fd;
        job->conn_id = conn_id;
        job->text_to_embed = strdup(clean_prompt);
        // Non serve key_part_1 per delete semantic, basta il vettore

        if (wp_submit(server->worker_pool, job) != 0) {
             // Fallback se coda piena: rispondi OK lo stesso (L1 è cancellato)
             // o manda errore. Per robustezza, mandiamo errore.
             buffer_append_string(write_buf, "-ERR Job Queue Full\r\n");
             free(job->text_to_embed); free(job);
        }
        
        // Attendiamo worker per la risposta definitiva
        return;
    }

    // --- COMANDO FLUSH ---
    else if (strcasecmp(argv[0], "FLUSH") == 0) {
        hash_map_clear(l1_cache);
        l2_cache_clear(server->l2_cache);
        log_info("FLUSH: Cache L1 e L2 svuotate.");
        buffer_append_string(write_buf, "+OK\r\n");
        el_enable_write(server->loop, fd, (void*)conn);
    }

    // --- COMANDO SAVE ---
    else if (strcasecmp(argv[0], "SAVE") == 0) {
        // SAVE rimane sincrono per ora (blocca il server per sicurezza dati)
        // In futuro si può fare fork() come Redis
        server_save_data(server);
        buffer_append_string(write_buf, "+OK\r\n");
        el_enable_write(server->loop, fd, (void*)conn);
    }
    
    // --- COMANDO SCONOSCIUTO ---
    else {
        snprintf(header_buf, sizeof(header_buf), "-ERR unknown command '%s'\r\n", argv[0]);
        buffer_append_string(write_buf, header_buf);
        el_enable_write(server->loop, fd, (void*)conn);
    }
}

vector_engine_t *server_get_engine(vecs_server_t *server) {
    return server->vec_engine;
}

static int get_optimal_worker_count(void) {
    // 1. Controlla ENV
    const char *val = getenv("VECS_NUM_WORKERS");
    if (val) {
        int env_count = atoi(val);
        if (env_count > 0) {
            return env_count;
        }
    }

    // 2. Controlla CPU Cores (nproc)
    long nprocs = -1;
#ifdef _SC_NPROCESSORS_ONLN
    nprocs = sysconf(_SC_NPROCESSORS_ONLN);
#endif

    if (nprocs > 0) {
        return (int)nprocs;
    }

    // 3. Fallback di sicurezza
    return 4;
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
    server->config.default_ttl = get_env_int("VECS_TTL_DEFAULT", DEFAULT_TTL);
    server->config.save_interval_seconds = get_env_int("VECS_SAVE_INTERVAL", DEFAULT_SAVE_INTERVAL);
    server->config.num_workers = get_optimal_worker_count();
    server->last_save_time = time(NULL);

    log_info("=== VECS CONFIG ===");
    log_info("Model Path:   %s", server->config.model_path);
    log_info("L2 Threshold: %.2f", server->config.l2_threshold);
    log_info("L2 Dedupe:    %.2f", server->config.l2_dedupe_threshold);
    log_info("L2 Capacity:  %d vectors", server->config.l2_capacity);
    log_info("Default TTL:  %d seconds", server->config.default_ttl);
    log_info("Auto-Save:    Every %d seconds", server->config.save_interval_seconds);
    log_info("AI Workers:   %d threads", server->config.num_workers);
    log_info("==================");

    // CREAZIONE CARTELLA DATI
    struct stat st = {0};
    if (stat(DUMP_DIR, &st) == -1) {
        // 0700 = rwx------ (Solo il proprietario può leggere/scrivere/entrare)
        // Se sei su Docker, questo corrisponde all'utente che esegue il processo (spesso root o app user)
        if (mkdir(DUMP_DIR, 0700) == -1) {
            log_fatal("Impossibile creare la directory '%s': %s", DUMP_DIR, strerror(errno));
            free(server->events);
            free(server);
            return NULL;
        }
        log_info("Creata directory dati: ./%s", DUMP_DIR);
    }
    
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

    vecs_engine_config_t eng_conf = {0};
    eng_conf.model_path = server->config.model_path;
    eng_conf.num_threads = server->config.num_workers;

    const char *mode_env = getenv("VECS_EXECUTION_MODE");
    if (mode_env && strcasecmp(mode_env, "gpu") == 0) {
        eng_conf.mode = VECS_MODE_GPU;
        eng_conf.gpu_layers = get_env_int("VECS_GPU_LAYERS", "99"); // Default offload tutto
        log_info("Mode: GPU Acceleration Enabled (Layers: %d)", eng_conf.gpu_layers);
    } else {
        eng_conf.mode = VECS_MODE_CPU;
        eng_conf.gpu_layers = 0;
        log_info("Mode: CPU Optimized");
    }

    // 3. AI Vector Engine
    log_info("Caricamento modello AI...");
    int num_workers = server->config.num_workers;
    int queue_limit = 1000;
    server->vec_engine = vector_engine_init(&eng_conf);
    if (!server->vec_engine) {
        log_fatal("ERRORE CRITICO: Impossibile caricare il modello GGUF da '%s'.", server->config.model_path);
        // Clean up parziale...
        return NULL;
    }

    // Crea pool con 4 worker (o pari a nproc)
    server->worker_pool = wp_create(server, num_workers, queue_limit);
    // Aggiungi la PIPE di notifica all'Event Loop
    int notify_fd = wp_get_notify_fd(server->worker_pool);
    if (el_add_fd_read(server->loop, notify_fd, (void*)server) == -1) { // Nota: udata è server per distinguerlo
        log_fatal("Impossibile aggiungere notify_fd al loop");
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

    server_load_data(server);
    log_info("Vecs Server avviato. Listening :%s. Vector Dim: %d", port, server->vector_dim);
    return server;
}

void server_destroy(vecs_server_t *server) {
    if (server == NULL) return;
    server_save_data(server);

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

    wp_destroy(server->worker_pool);
    el_destroy(server->loop);
    free(server->events);
    free(server);

    log_info("Server terminato.");
}

int server_run(vecs_server_t *server) {
    log_info("Loop eventi in esecuzione...");

    while (1) {
        int num_events = el_poll(server->loop, server->events, 1000);

        if (num_events == -1) {
            if (errno == EINTR) continue;
            log_error("Errore critico el_poll: %s", strerror(errno));
            return -1;
        }

        for (int i = 0; i < num_events; i++) {
            vecs_event_t *event = &server->events[i];

            int notify_fd = wp_get_notify_fd(server->worker_pool);
            
            if (event->fd == server->listen_fd) {
             server_handle_new_connection(server);
            } 
            else if (event->fd == notify_fd) {
                server_handle_worker_notification(server);
            }
            else {
                server_handle_client_event(event);
            }
        }

        if (server->config.save_interval_seconds > 0) {
            time_t now = time(NULL);
            if (now - server->last_save_time >= server->config.save_interval_seconds) {
                // È ora di salvare!
                log_debug("Auto-save timer scattato.");
                server_save_data(server);
                server->last_save_time = now; // Resetta timer
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

static void server_save_data(vecs_server_t *server) {
    log_info("Salvataggio dati su disco (%s)...", DUMP_FILENAME);
    FILE *f = fopen(DUMP_FILENAME, "wb");
    if (!f) {
        log_error("Impossibile aprire file dump per scrittura: %s", strerror(errno));
        return;
    }

    // Magic Header "VECS01"
    fwrite("VECS01", 1, 6, f);

    hash_map_save(server->l1_cache, f);
    l2_cache_save(server->l2_cache, f);

    fclose(f);
    log_info("Salvataggio completato.");
}

static void server_load_data(vecs_server_t *server) {
    if (access(DUMP_FILENAME, F_OK) == -1) {
        log_info("Nessun file dump trovato. Avvio a vuoto.");
        return;
    }

    log_info("Caricamento dati da %s...", DUMP_FILENAME);
    FILE *f = fopen(DUMP_FILENAME, "rb");
    if (!f) return;

    char magic[7] = {0};
    fread(magic, 1, 6, f);
    if (strcmp(magic, "VECS01") != 0) {
        log_error("Header file dump non valido o versione errata.");
        fclose(f);
        return;
    }

    hash_map_load(server->l1_cache, f);
    l2_cache_load(server->l2_cache, f);

    fclose(f);
}

static void server_handle_worker_notification(vecs_server_t *server) {
    while (1) {
        // 1. Legge il puntatore al job dalla pipe
        bg_job_t *job = wp_read_completed_job(server->worker_pool);
        if (!job) break; // Pipe vuota, nessun altro lavoro completato

        // 2. Recupera la connessione associata
        // Attenzione: dobbiamo verificare che esista ancora e che sia LA STESSA connessione
        // (il socket potrebbe essere stato chiuso e riaperto da un altro client)
        if (job->client_fd < 0 || job->client_fd >= MAX_FD) {
            log_warn("Async Job: FD non valido (%d)", job->client_fd);
            goto cleanup;
        }

        vecs_connection_t *conn = server->connections[job->client_fd];

        // Se la connessione è nulla o l'ID non corrisponde, il client si è disconnesso nel frattempo.
        if (!conn || connection_get_id(conn) != job->conn_id) {
            log_info("Async Job ignorato: il client (fd %d) si è disconnesso.", job->client_fd);
            goto cleanup;
        }

        buffer_t *write_buf = connection_get_write_buffer(conn);
        char header_buf[64];

        if (!job->success) {
            // Caso Errore nel Worker (es. fallimento allocazione o modello)
            buffer_append_string(write_buf, "-ERR Vector Embedding Failed\r\n");
        } else {
            // --- LOGICA SPECIFICA PER TIPO DI JOB ---

            if (job->type == JOB_SET) {
                // Il vettore è calcolato. Ora facciamo la DEDUPLICA e INSERIMENTO L2.
                // Questo avviene nel Main Thread, quindi è thread-safe per la cache.
                
                const char *existing = l2_cache_search(
                    server->l2_cache, 
                    job->vector_result, 
                    job->key_part_1, // Il prompt originale
                    server->config.l2_dedupe_threshold
                );
                
                if (existing != NULL) {
                    log_info("Async SET L2 Skipped: Concetto già presente.");
                } else {
                    l2_cache_insert(server->l2_cache, job->vector_result, job->key_part_1, job->value, job->ttl);
                    log_info("Async SET L2 OK.");
                }
                
                buffer_append_string(write_buf, "+OK\r\n");

            } else if (job->type == JOB_QUERY) {
                // Il vettore query è pronto. Eseguiamo la ricerca L2.
                
                const char *semantic_val = l2_cache_search(
                    server->l2_cache,
                    job->vector_result,
                    job->key_part_1, // Il prompt originale (usato per i filtri text-based)
                    server->config.l2_threshold
                );

                if (semantic_val != NULL) {
                    // HIT L2
                    size_t val_len = strlen(semantic_val);
                    snprintf(header_buf, sizeof(header_buf), "$%zu\r\n", val_len);
                    buffer_append_string(write_buf, header_buf);
                    buffer_append_data(write_buf, semantic_val, val_len);
                    buffer_append_string(write_buf, "\r\n");
                    log_info("Async HIT L2 (Semantic)");
                } else {
                    // MISS L2
                    buffer_append_string(write_buf, "$-1\r\n");
                    log_debug("Async MISS L2");
                }

            } else if (job->type == JOB_DELETE) {
                // Il vettore del prompt da cancellare è pronto.
                int deleted = l2_cache_delete_semantic(server->l2_cache, job->vector_result);
                log_info("Async DELETE L2 completed. Removed: %d", deleted);
                buffer_append_string(write_buf, "+OK\r\n");
            }
        }

        // 3. Abilita la scrittura sul socket per inviare la risposta al client
        el_enable_write(server->loop, job->client_fd, (void*)conn);

cleanup:
        // 4. Libera tutta la memoria del Job
        if (job->text_to_embed) free(job->text_to_embed);
        if (job->key_part_1) free(job->key_part_1);
        if (job->key_part_2) free(job->key_part_2);
        if (job->value) free(job->value);
        if (job->vector_result) free(job->vector_result);
        free(job);
    }
}