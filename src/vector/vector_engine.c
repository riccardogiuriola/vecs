/*
 * Vecs Project: Vector Engine (Wrapper Llama.cpp)
 * (src/vector/vector_engine.c)
 * VERSION: STABILITY FIX (Race Condition Prevention)
 */

#include "vector_engine.h"
#include "llama.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>

// --- STRUTTURE PER SCHEDULER GPU ---

typedef struct gpu_request_s {
    llama_token *tokens;
    int n_tokens;
    float *output_dest;
    
    pthread_cond_t cond;
    pthread_mutex_t mutex;  
    int done;
    int success;

    struct gpu_request_s *next;
} gpu_request_t;

struct vector_engine_s {
    struct llama_model *model;
    int n_embd;
    int is_bert;
    vecs_execution_mode_t mode;

    // CPU
    struct llama_context **cpu_ctxs;
    struct llama_batch *cpu_batches;
    int num_cpu_threads;

    // GPU
    struct llama_context *gpu_ctx;
    struct llama_batch gpu_batch;
    int gpu_batch_capacity;
    int gpu_max_seq;
    
    pthread_t scheduler_tid;
    int scheduler_running;
    
    gpu_request_t *queue_head;
    gpu_request_t *queue_tail;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
};

// --- PROTOTIPI ---
static void* gpu_scheduler_loop(void *arg);
static int embed_cpu(vector_engine_t *engine, int thread_id, const char *text, float *out);
static int embed_gpu(vector_engine_t *engine, const char *text, float *out);
static void str_tolower(char *str);

// --- HELPER ---
static void str_tolower(char *str) {
    for(; *str; ++str) *str = tolower(*str);
}

// --- INIT ---

vector_engine_t* vector_engine_init(vecs_engine_config_t *config) {
    llama_backend_init();

    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config->gpu_layers;

    struct llama_model *model = llama_model_load_from_file(config->model_path, model_params);
    if (!model) {
        log_fatal("Fallito caricamento modello: %s", config->model_path);
        return NULL;
    }

    vector_engine_t *engine = calloc(1, sizeof(vector_engine_t));
    engine->model = model;
    engine->n_embd = llama_model_n_embd(model);
    engine->mode = config->mode;

    // --- RILEVAMENTO BERT ---
    engine->is_bert = (llama_model_has_encoder(model) != 0); 
    if (!engine->is_bert) {
        char path_lower[512];
        strncpy(path_lower, config->model_path, 511);
        str_tolower(path_lower);
        if (strstr(path_lower, "bge") || strstr(path_lower, "bert")) {
            engine->is_bert = 1;
            log_warn("Rilevato BGE/BERT dal nome file.");
        }
    }

    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.embeddings = true;

    if (engine->mode == VECS_MODE_CPU) {
        // SETUP CPU
        engine->num_cpu_threads = config->num_threads;
        engine->cpu_ctxs = calloc(config->num_threads, sizeof(struct llama_context*));
        engine->cpu_batches = calloc(config->num_threads, sizeof(struct llama_batch));
        ctx_params.n_ctx = 512; 

        for (int i = 0; i < config->num_threads; i++) {
            engine->cpu_ctxs[i] = llama_init_from_model(model, ctx_params);
            engine->cpu_batches[i] = llama_batch_init(512, 0, 1);
        }
    } 
    else {
        // SETUP GPU
        int max_batch_tokens = 4096; 
        engine->gpu_batch_capacity = max_batch_tokens;

        ctx_params.n_ctx = max_batch_tokens;
        ctx_params.n_batch = max_batch_tokens; 
        
        // --- LIMITE DINAMICO SEQUENZE ---
        #if defined(__APPLE__) || defined(__aarch64__)
            // Su ARM64/Metal spesso il limite hardware o della lib è più basso per i batch
            engine->gpu_max_seq = 256;
            log_info("Platform: ARM64/Metal detected. Setting Max Seq = 256.");
        #else
            // Linux x86_64 (Intel/AMD) con GPU NVIDIA di solito supporta batch enormi
            engine->gpu_max_seq = max_batch_tokens;
            log_info("Platform: x86_64/CUDA. Setting Max Seq = %d.", max_batch_tokens);
        #endif

        ctx_params.n_seq_max = engine->gpu_max_seq;

        engine->gpu_ctx = llama_init_from_model(model, ctx_params);
        if (!engine->gpu_ctx) return NULL;

        engine->gpu_batch = llama_batch_init(max_batch_tokens, 0, engine->gpu_max_seq);

        pthread_mutex_init(&engine->queue_lock, NULL);
        pthread_cond_init(&engine->queue_cond, NULL);
        engine->scheduler_running = 1;

        pthread_create(&engine->scheduler_tid, NULL, gpu_scheduler_loop, engine);
        log_info("GPU Scheduler: READY (IsBert: %d, BatchCap: %d, MaxSeq: %d)", 
                 engine->is_bert, max_batch_tokens, engine->gpu_max_seq);
    }

    return engine;
}

void vector_engine_destroy(vector_engine_t *engine) {
    if (!engine) return;

    if (engine->mode == VECS_MODE_CPU) {
        for (int i = 0; i < engine->num_cpu_threads; i++) {
            if (engine->cpu_batches) llama_batch_free(engine->cpu_batches[i]);
            if (engine->cpu_ctxs && engine->cpu_ctxs[i]) llama_free(engine->cpu_ctxs[i]);
        }
        if (engine->cpu_batches) free(engine->cpu_batches);
        if (engine->cpu_ctxs) free(engine->cpu_ctxs);
    } 
    else {
        if (engine->scheduler_running) {
            pthread_mutex_lock(&engine->queue_lock);
            engine->scheduler_running = 0;
            pthread_cond_signal(&engine->queue_cond);
            pthread_mutex_unlock(&engine->queue_lock);
            pthread_join(engine->scheduler_tid, NULL);
        }
        llama_batch_free(engine->gpu_batch);
        if (engine->gpu_ctx) llama_free(engine->gpu_ctx);
        pthread_mutex_destroy(&engine->queue_lock);
        pthread_cond_destroy(&engine->queue_cond);
    }

    if (engine->model) llama_model_free(engine->model);
    llama_backend_free();
    free(engine);
}

int vector_engine_get_dim(vector_engine_t *engine) {
    return engine->n_embd;
}

int vector_engine_embed(vector_engine_t *engine, int thread_id, const char *text, float *out_vector) {
    if (engine->mode == VECS_MODE_CPU) {
        return embed_cpu(engine, thread_id, text, out_vector);
    } else {
        return embed_gpu(engine, text, out_vector);
    }
}

// --- IMPLEMENTAZIONE CPU ---

static int embed_cpu(vector_engine_t *engine, int thread_id, const char *text, float *out_vector) {
    if (!engine || !text || !out_vector) return -1;
    if (thread_id < 0 || thread_id >= engine->num_cpu_threads) return -1;

    struct llama_context *ctx = engine->cpu_ctxs[thread_id];
    struct llama_batch *batch = &engine->cpu_batches[thread_id];

    const struct llama_vocab *vocab = llama_model_get_vocab(engine->model);
    int n_tokens_alloc = strlen(text) + 4;
    llama_token *tokens = malloc(n_tokens_alloc * sizeof(llama_token));
    
    int n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_tokens_alloc, true, false);
    if (n_tokens < 0) {
        n_tokens_alloc = -n_tokens;
        tokens = realloc(tokens, n_tokens_alloc * sizeof(llama_token));
        n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_tokens_alloc, true, false);
    }

    if (n_tokens <= 0) { free(tokens); return -1; }
    if (n_tokens > 512) n_tokens = 512;

    batch->n_tokens = n_tokens;
    for (int i = 0; i < n_tokens; i++) {
        batch->token[i] = tokens[i];
        batch->pos[i] = i;
        batch->seq_id[i][0] = 0;
        batch->n_seq_id[i] = 1;
        batch->logits[i] = (i == n_tokens - 1);
    }

    // Compatibilità API
    llama_memory_t mem = llama_get_memory(ctx);
    llama_memory_seq_rm(mem, -1, -1, -1);

    int ret = 0;
    if (engine->is_bert) ret = llama_encode(ctx, *batch);
    else ret = llama_decode(ctx, *batch);

    if (ret != 0) { free(tokens); return -1; }

    float *emb = NULL;
    if (engine->is_bert) emb = llama_get_embeddings_ith(ctx, 0); 
    else emb = llama_get_embeddings_seq(ctx, 0);
    
    if (emb) {
        memcpy(out_vector, emb, engine->n_embd * sizeof(float));
        float norm = 0.0f;
        for (int i = 0; i < engine->n_embd; i++) norm += out_vector[i] * out_vector[i];
        norm = sqrtf(norm);
        if (norm > 1e-9) for (int i = 0; i < engine->n_embd; i++) out_vector[i] /= norm;
    } else {
        memset(out_vector, 0, engine->n_embd * sizeof(float));
    }

    free(tokens);
    return 0;
}

// --- IMPLEMENTAZIONE GPU ---

static int embed_gpu(vector_engine_t *engine, const char *text, float *out) {
    int n_alloc = strlen(text) + 5;
    llama_token *tokens = malloc(n_alloc * sizeof(llama_token));
    const struct llama_vocab *vocab = llama_model_get_vocab(engine->model);
    int n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_alloc, true, false);
    
    if (n_tokens < 0) {
        n_alloc = -n_tokens;
        tokens = realloc(tokens, n_alloc * sizeof(llama_token));
        n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_alloc, true, false);
    }

    if (n_tokens <= 0) { free(tokens); return -1; }
    
    if (n_tokens > engine->gpu_batch_capacity) n_tokens = engine->gpu_batch_capacity;

    gpu_request_t req;
    req.tokens = tokens;
    req.n_tokens = n_tokens;
    req.output_dest = out;
    req.done = 0;
    req.success = 0;
    req.next = NULL;
    pthread_mutex_init(&req.mutex, NULL);
    pthread_cond_init(&req.cond, NULL);

    pthread_mutex_lock(&engine->queue_lock);
    if (engine->queue_tail) {
        engine->queue_tail->next = &req;
    } else {
        engine->queue_head = &req;
    }
    engine->queue_tail = &req;
    pthread_cond_signal(&engine->queue_cond);
    pthread_mutex_unlock(&engine->queue_lock);

    // WAIT DEL WORKER
    pthread_mutex_lock(&req.mutex);
    while (!req.done) {
        pthread_cond_wait(&req.cond, &req.mutex);
    }
    pthread_mutex_unlock(&req.mutex);

    // CLEANUP
    // A questo punto lo scheduler ha finito con noi. 
    // Possiamo liberare tutto in sicurezza.
    free(tokens);
    pthread_mutex_destroy(&req.mutex);
    pthread_cond_destroy(&req.cond);

    return req.success ? 0 : -1;
}

static void* gpu_scheduler_loop(void *arg) {
    vector_engine_t *engine = (vector_engine_t*)arg;

    while (engine->scheduler_running) {
        // 1. Attesa job
        pthread_mutex_lock(&engine->queue_lock);
        while (engine->queue_head == NULL && engine->scheduler_running) {
            pthread_cond_wait(&engine->queue_cond, &engine->queue_lock);
        }
        
        if (!engine->scheduler_running) {
            pthread_mutex_unlock(&engine->queue_lock);
            break;
        }

        // 2. Preleviamo tutta la coda
        gpu_request_t *full_list = engine->queue_head;
        engine->queue_head = NULL;
        engine->queue_tail = NULL;
        pthread_mutex_unlock(&engine->queue_lock);

        gpu_request_t *cursor = full_list;
        
        while (cursor) {
            int n_tokens_batch = 0;
            int n_seq_in_batch = 0;
            
            engine->gpu_batch.n_tokens = 0;
            gpu_request_t *batch_start = cursor;
            gpu_request_t *batch_end = NULL;
            int seq_id = 0;

            // 3. Creazione sotto-batch
            while (cursor) {
                if (n_tokens_batch + cursor->n_tokens > engine->gpu_batch_capacity) break;
                if (n_seq_in_batch >= engine->gpu_max_seq) break;

                for (int i = 0; i < cursor->n_tokens; i++) {
                    int pos = engine->gpu_batch.n_tokens;
                    engine->gpu_batch.token[pos] = cursor->tokens[i];
                    engine->gpu_batch.pos[pos] = i;
                    engine->gpu_batch.n_seq_id[pos] = 1;
                    engine->gpu_batch.seq_id[pos][0] = seq_id;
                    engine->gpu_batch.logits[pos] = (i == cursor->n_tokens - 1);
                    engine->gpu_batch.n_tokens++;
                }
                
                n_tokens_batch += cursor->n_tokens;
                n_seq_in_batch++;
                seq_id++;
                batch_end = cursor;
                cursor = cursor->next;
            }

            if (n_tokens_batch > 0) {
                // Clear Memory
                llama_memory_t mem = llama_get_memory(engine->gpu_ctx);
                llama_memory_seq_rm(mem, -1, -1, -1);

                int ret = -1;
                if (engine->is_bert) {
                    ret = llama_encode(engine->gpu_ctx, engine->gpu_batch);
                } else {
                    ret = llama_decode(engine->gpu_ctx, engine->gpu_batch);
                }

                if (ret != 0) {
                    log_error("GPU Inference Failed (ret=%d)", ret);
                }

                // 4. Distribuzione Risultati (FIX SEGFAULT QUI)
                gpu_request_t *notify_cursor = batch_start;
                int current_seq = 0;
                
                while (notify_cursor) {
                    // --- FIX CRITICO PER SEGFAULT ---
                    // Salviamo il puntatore 'next' PRIMA di svegliare il worker.
                    // Appena svegliamo il worker (pthread_cond_signal + unlock),
                    // il worker potrebbe uscire e distruggere la struct 'notify_cursor'.
                    gpu_request_t *safe_next = notify_cursor->next;
                    int is_last_in_batch = (notify_cursor == batch_end);

                    pthread_mutex_lock(&notify_cursor->mutex);
                    
                    if (ret == 0) {
                        float *emb = NULL;
                        emb = llama_get_embeddings_seq(engine->gpu_ctx, current_seq);
                        
                        if (!emb && engine->is_bert) {
                             emb = llama_get_embeddings_ith(engine->gpu_ctx, current_seq);
                        }

                        if (emb) {
                            memcpy(notify_cursor->output_dest, emb, engine->n_embd * sizeof(float));
                            float norm = 0.0f;
                            for(int k=0; k<engine->n_embd; k++) norm += emb[k]*emb[k];
                            norm = sqrtf(norm);
                            if(norm > 1e-9) for(int k=0; k<engine->n_embd; k++) notify_cursor->output_dest[k] /= norm;
                            notify_cursor->success = 1;
                        } else {
                            notify_cursor->success = 0;
                        }
                    } else {
                        notify_cursor->success = 0;
                    }
                    
                    notify_cursor->done = 1;
                    pthread_cond_signal(&notify_cursor->cond);
                    pthread_mutex_unlock(&notify_cursor->mutex); // <-- Worker si sveglia qui

                    // Da qui in poi, NON toccare più 'notify_cursor'
                    if (is_last_in_batch) break;
                    notify_cursor = safe_next; // Usiamo il puntatore salvato
                    current_seq++;
                }
            }
        }
    }
    return NULL;
}