/*
 * Vecs Project: Vector Engine (Wrapper Llama.cpp)
 * (src/vector/vector_engine.c)
 * VERSION: CPU-ONLY SAFE MODE + DEBUG LOGGING
 */

#include "vector_engine.h"
#include "llama.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct vector_engine_s {
    struct llama_model *model;
    
    struct llama_context **ctxs;    
    struct llama_batch *batches;
    int num_threads;
    
    int n_embd;
    int batch_capacity;
    int is_bert; 
};

vector_engine_t* vector_engine_init(const char *model_path, int num_threads) {
    llama_backend_init();

    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 0; 

    struct llama_model *model = llama_model_load_from_file(model_path, model_params);
    if (!model) {
        log_error("Fallito caricamento modello: %s", model_path);
        return NULL;
    }

    vector_engine_t *engine = calloc(1, sizeof(vector_engine_t));
    engine->model = model;
    engine->n_embd = llama_model_n_embd(model); 
    engine->num_threads = num_threads;
    engine->batch_capacity = 2048;

    // --- Rilevamento Architettura (BERT vs Llama) ---
    engine->is_bert = 0;
    if (llama_model_has_encoder(model)) {
        engine->is_bert = 1;
    } else {
        char arch[128] = {0};
        if (llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch)) > 0) {
            if (strstr(arch, "bert") || strstr(arch, "nomic-bert") || strstr(arch, "roberta")) {
                engine->is_bert = 1;
            }
        }
    }

    // --- Allocazione Risorse Thread ---
    engine->ctxs = calloc(num_threads, sizeof(struct llama_context*));
    engine->batches = calloc(num_threads, sizeof(struct llama_batch));

    // Configurazione Contesto (DEFINITA UNA VOLTA SOLA)
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 2048; 
    ctx_params.embeddings = true;

    for (int i = 0; i < num_threads; i++) {
        engine->ctxs[i] = llama_init_from_model(model, ctx_params);
        if (!engine->ctxs[i]) {
            log_fatal("Impossibile creare contesto llama per thread %d (OOM?)", i);
            return NULL; 
        }
        engine->batches[i] = llama_batch_init(engine->batch_capacity, 0, 1);
    }

    log_info("Vector Engine Init: Dim=%d, Mode=%s, Threads=%d, Backend=CPU", 
             engine->n_embd, 
             engine->is_bert ? "Encoder (BERT/BGE)" : "Decoder (Llama/GPT)",
             num_threads);
    
    return engine;
}

void vector_engine_destroy(vector_engine_t *engine) {
    if (!engine) return;

    for (int i = 0; i < engine->num_threads; i++) {
        if (engine->batches) llama_batch_free(engine->batches[i]);
        if (engine->ctxs && engine->ctxs[i]) llama_free(engine->ctxs[i]);
    }

    if (engine->batches) free(engine->batches);
    if (engine->ctxs) free(engine->ctxs);
    if (engine->model) llama_model_free(engine->model);

    llama_backend_free();
    free(engine);
}

int vector_engine_get_dim(vector_engine_t *engine) {
    return engine->n_embd;
}

int vector_engine_embed(vector_engine_t *engine, int thread_id, const char *text, float *out_vector) {
    if (!engine || !text || !out_vector) return -1;
    if (thread_id < 0 || thread_id >= engine->num_threads) {
        log_error("Invalid thread_id: %d", thread_id);
        return -1;
    }

    struct llama_context *ctx = engine->ctxs[thread_id];
    struct llama_batch *batch = &engine->batches[thread_id];

    // --- 1. Tokenizzazione ---
    const struct llama_vocab *vocab = llama_model_get_vocab(engine->model);
    int n_tokens_alloc = strlen(text) + 4; // Un po' di buffer extra
    llama_token *tokens = malloc(n_tokens_alloc * sizeof(llama_token));
    
    // add_special=true per BERT/BGE
    int n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_tokens_alloc, true, false);

    if (n_tokens < 0) {
        // Resize buffer se serve
        n_tokens_alloc = -n_tokens;
        tokens = realloc(tokens, n_tokens_alloc * sizeof(llama_token));
        n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_tokens_alloc, true, false);
    }

    // DEBUG TOKENIZER: Qui era il fallimento silenzioso!
    if (n_tokens <= 0) {
        log_error("Tokenizer failed for text: '%.20s...' (Len: %lu). Ret: %d", text, strlen(text), n_tokens);
        free(tokens);
        return -1;
    }

    if (n_tokens > engine->batch_capacity) {
        log_warn("Truncating input: %d tokens > batch capacity %d", n_tokens, engine->batch_capacity);
        n_tokens = engine->batch_capacity;
    }

    // --- 2. Preparazione Batch ---
    // Importante: Reset del batch
    batch->n_tokens = n_tokens;
    for (int i = 0; i < n_tokens; i++) {
        batch->token[i] = tokens[i];
        batch->pos[i] = i;
        batch->seq_id[i][0] = 0;
        batch->n_seq_id[i] = 1;
        batch->logits[i] = true; 
    }

    // --- 3. Inferenza ---
    llama_memory_t mem = llama_get_memory(ctx);
    llama_memory_seq_rm(mem, -1, -1, -1);

    int ret = 0;
    if (engine->is_bert) {
        ret = llama_encode(ctx, *batch);
    } else {
        ret = llama_decode(ctx, *batch);
    }

    if (ret != 0) {
        log_error("Llama Inference Failed (Thread %d). Code: %d. Prompt: '%.10s...'", thread_id, ret, text);
        free(tokens);
        return -1;
    }

    // --- 4. Estrazione Embedding ---
    float *emb = llama_get_embeddings_seq(ctx, 0);
    
    if (emb) {
        memcpy(out_vector, emb, engine->n_embd * sizeof(float));
    } else {
        // Fallback Manual Pooling
        if (engine->is_bert) {
            float *cls = llama_get_embeddings_ith(ctx, 0);
            if (cls) memcpy(out_vector, cls, engine->n_embd * sizeof(float));
        } else {
            memset(out_vector, 0, engine->n_embd * sizeof(float));
            int count = 0;
            for (int i = 0; i < n_tokens; i++) {
                float *t = llama_get_embeddings_ith(ctx, i);
                if (t) {
                    for (int j = 0; j < engine->n_embd; j++) out_vector[j] += t[j];
                    count++;
                }
            }
            if (count > 0) {
                for (int j = 0; j < engine->n_embd; j++) out_vector[j] /= (float)count;
            }
        }
    }

    // --- 5. Normalizzazione ---
    float norm = 0.0f;
    for (int i = 0; i < engine->n_embd; i++) norm += out_vector[i] * out_vector[i];
    norm = sqrtf(norm);
    if (norm > 1e-9) {
        for (int i = 0; i < engine->n_embd; i++) out_vector[i] /= norm;
    }

    free(tokens);
    return 0;
}