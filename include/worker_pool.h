#ifndef VECS_WORKER_POOL_H
#define VECS_WORKER_POOL_H

#include "server.h"
#include "connection.h"

typedef struct worker_pool_s worker_pool_t;

// Tipi di Task
typedef enum
{
    JOB_SET,
    JOB_QUERY,
    JOB_DELETE
} job_type_t;

// Struttura del Job (Task)
typedef struct
{
    job_type_t type;

    // Contesto Connessione (per rispondere)
    int client_fd;
    uint64_t conn_id;

    // Dati Input
    char *text_to_embed;

    // Dati per SET
    char *key_part_1;
    char *key_part_2;
    char *value;
    int ttl;

    // Output (Calcolato dal Worker)
    float *vector_result;
    int success;

} bg_job_t;

// Inizializza il pool
worker_pool_t *wp_create(vecs_server_t *server, int num_workers, int max_queue_size);

// Distrugge il pool
void wp_destroy(worker_pool_t *pool);

// Invia un job alla coda (Thread Safe)
int wp_submit(worker_pool_t *pool, bg_job_t *job);

// Ottiene il File Descriptor della Pipe di notifica (da aggiungere all'event loop)
int wp_get_notify_fd(worker_pool_t *pool);

// Legge dalla pipe un job completato
bg_job_t *wp_read_completed_job(worker_pool_t *pool);

#endif