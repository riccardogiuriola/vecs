#include "worker_pool.h"
#include "vector_engine.h"
#include "logger.h"
#include "server.h" 
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

// Nodo della coda
typedef struct job_node_s {
    bg_job_t *job;
    struct job_node_s *next;
} job_node_t;

struct worker_pool_s {
    vecs_server_t *server;
    pthread_t *threads;
    int num_workers;
    int running; 

    // Coda Job
    job_node_t *head;
    job_node_t *tail;
    
    // Limiti Coda
    int max_jobs;
    int current_jobs;
    
    // Sincronizzazione
    pthread_mutex_t lock;
    pthread_cond_t cond;

    // Pipe per notifica (0: read, 1: write)
    int pipe_fd[2];
};

// Struttura per passare argomenti ai thread
typedef struct {
    worker_pool_t *pool;
    int thread_id;
} worker_arg_t;

// Funzione Worker Thread
static void *worker_routine(void *arg) {
    // 1. Unpack argomenti
    worker_arg_t *w_arg = (worker_arg_t *)arg;
    worker_pool_t *pool = w_arg->pool;
    int my_id = w_arg->thread_id;
    free(w_arg);

    //log_debug("Worker %d: Thread avviato.", my_id);

    while (1) {
        bg_job_t *job = NULL;

        // 2. Dequeue (Thread Safe)
        pthread_mutex_lock(&pool->lock);
        while (pool->head == NULL && pool->running) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        if (!pool->running) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        job_node_t *node = pool->head;
        job = node->job;
        pool->head = node->next;
        if (pool->head == NULL) pool->tail = NULL;
        pool->current_jobs--;
        free(node);
        pthread_mutex_unlock(&pool->lock);

        // 3. ESECUZIONE EMBEDDING
        log_debug("Worker %d: Job %lu preso. Allocazione memoria...", my_id, job->conn_id);
        
        vector_engine_t *engine = server_get_engine(pool->server);
        
        // ALLOCAZIONE MEMORIA VETTORE ---
        int dim = vector_engine_get_dim(engine);
        job->vector_result = malloc(dim * sizeof(float));
        
        if (!job->vector_result) {
            log_error("Worker %d: OOM allocazione vettore risultato", my_id);
            job->success = 0;
        } else {
            // Ora vector_result è valido, l'engine non ritornerà -1 subito
            int ret = vector_engine_embed(engine, my_id, job->text_to_embed, job->vector_result);
            job->success = (ret == 0) ? 1 : 0;
        }
        // ----------------------------------------

        if (job->success) {
            log_debug("Worker %d: Embedding OK.", my_id);
        } else {
            log_error("Worker %d: Embedding FALLITO.", my_id);
            // Cleanup in caso di fallimento parziale
            if (job->vector_result) {
                free(job->vector_result);
                job->vector_result = NULL;
            }
        }

        // 4. Notifica
        if (write(pool->pipe_fd[1], &job, sizeof(bg_job_t *)) == -1) {
             log_error("Worker %d: Errore pipe", my_id);
        }
    }
    return NULL;
}

worker_pool_t *wp_create(vecs_server_t *server, int num_workers, int max_queue_size) {
    worker_pool_t *pool = calloc(1, sizeof(worker_pool_t));
    if (!pool) return NULL;

    pool->server = server;
    pool->num_workers = num_workers;
    pool->running = 1;
    pool->max_jobs = max_queue_size;
    pool->current_jobs = 0;

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);

    // Creazione Pipe
    if (pipe(pool->pipe_fd) == -1) {
        log_fatal("Impossibile creare pipe worker: %s", strerror(errno));
        free(pool);
        return NULL;
    }

    // Imposta ENTRAMBI i lati come non-bloccanti
    int flags = fcntl(pool->pipe_fd[0], F_GETFL, 0);
    fcntl(pool->pipe_fd[0], F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(pool->pipe_fd[1], F_GETFL, 0);
    fcntl(pool->pipe_fd[1], F_SETFL, flags | O_NONBLOCK);

    // Avvio Thread
    pool->threads = calloc(num_workers, sizeof(pthread_t));
    for (int i = 0; i < num_workers; i++) {
        // Creiamo una struct argomenti per ogni thread
        worker_arg_t *arg = malloc(sizeof(worker_arg_t));
        arg->pool = pool;
        arg->thread_id = i;

        if (pthread_create(&pool->threads[i], NULL, worker_routine, arg) != 0) {
            log_fatal("Fallita creazione worker thread %d", i);
            free(arg);
        }
    }

    log_info("Worker Pool avviato con %d threads.", num_workers);
    return pool;
}

void wp_destroy(worker_pool_t *pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);
    pool->running = 0;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    close(pool->pipe_fd[0]);
    close(pool->pipe_fd[1]);
    
    // Cleanup coda residua (omesso per brevità, ma in prod andrebbe svuotata)
    
    free(pool->threads);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    free(pool);
}

int wp_submit(worker_pool_t *pool, bg_job_t *job) {
    if (!pool || !job) return -1;

    // Alloca nodo prima del lock
    job_node_t *node = malloc(sizeof(job_node_t));
    if (!node) return -1;
    node->job = job;
    node->next = NULL;

    pthread_mutex_lock(&pool->lock);
    
    // Controllo Backpressure
    if (pool->current_jobs >= pool->max_jobs) {
        pthread_mutex_unlock(&pool->lock);
        free(node);
        log_warn("Job rifiutato: Coda piena (%d jobs)", pool->max_jobs);
        return -1;
    }

    if (pool->tail) {
        pool->tail->next = node;
    } else {
        pool->head = node;
    }
    pool->tail = node;
    pool->current_jobs++;

    pthread_cond_signal(&pool->cond); // Sveglia un worker
    pthread_mutex_unlock(&pool->lock);
    
    return 0;
}

int wp_get_notify_fd(worker_pool_t *pool) {
    return pool->pipe_fd[0];
}

bg_job_t *wp_read_completed_job(worker_pool_t *pool) {
    bg_job_t *job = NULL;
    ssize_t n = read(pool->pipe_fd[0], &job, sizeof(bg_job_t *));
    if (n == sizeof(bg_job_t *)) {
        return job;
    }
    return NULL;
}