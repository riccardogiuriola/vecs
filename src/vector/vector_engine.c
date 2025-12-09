/*
 * Vex Project: Vector Engine (Wrapper Llama.cpp)
 * (src/vector/vector_engine.c)
 */

#include "vector_engine.h"
#include "llama.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

struct vector_engine_s {
    struct llama_model *model;
    struct llama_context *ctx;
    int n_embd;
};

vector_engine_t* vector_engine_init(const char *model_path) {
    llama_backend_init();

    struct llama_model_params model_params = llama_model_default_params();
    
    struct llama_model *model = llama_load_model_from_file(model_path, model_params);
    if (!model) model = llama_model_load_from_file(model_path, model_params);
    
    if (!model) {
        log_error("Fallito caricamento modello: %s", model_path);
        return NULL;
    }

    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 2048;
    ctx_params.embeddings = true;

    struct llama_context *ctx = llama_new_context_with_model(model, ctx_params);
    if (!ctx) ctx = llama_init_from_model(model, ctx_params);

    if (!ctx) {
        log_error("Fallita creazione contesto llama");
        llama_free_model(model);
        return NULL;
    }

    vector_engine_t *engine = malloc(sizeof(vector_engine_t));
    engine->model = model;
    engine->ctx = ctx;
    engine->n_embd = llama_n_embd(model); 
    
    log_info("Vector Engine inizializzato. Dimensione: %d", engine->n_embd);
    return engine;
}

void vector_engine_destroy(vector_engine_t *engine) {
    if (!engine) return;
    if (engine->ctx) llama_free(engine->ctx);
    if (engine->model) llama_free_model(engine->model);
    free(engine);
    llama_backend_free();
}

int vector_engine_get_dim(vector_engine_t *engine) {
    return engine->n_embd;
}

int vector_engine_embed(vector_engine_t *engine, const char *text, float *out_vector) {
    if (!engine || !text || !out_vector) return -1;

    const struct llama_vocab *vocab = llama_model_get_vocab(engine->model);

    int n_tokens_alloc = strlen(text) + 16; 
    llama_token *tokens = malloc(n_tokens_alloc * sizeof(llama_token));
    
    int n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_tokens_alloc, true, false);

    if (n_tokens < 0) {
        n_tokens_alloc = -n_tokens;
        tokens = realloc(tokens, n_tokens_alloc * sizeof(llama_token));
        n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_tokens_alloc, true, false);
    }

    if (n_tokens < 0) {
        log_error("Errore tokenizzazione");
        free(tokens);
        return -1;
    }

    // --- PULIZIA CACHE ---
    // Poiché llama_batch_get_one imposta le posizioni a partire da 0,
    // i vecchi dati nella cache verranno sovrascritti automaticamente.
    // llama_kv_cache_seq_rm(engine->ctx, -1, -1, -1); 

    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens);

    if (llama_model_has_encoder(engine->model) && llama_model_has_decoder(engine->model)) {
        // Modello encoder-decoder (raro per embedding puri, ma possibile)
        if (llama_decode(engine->ctx, batch) != 0) {
            log_error("llama_decode fallito");
            free(tokens);
            return -1;
        }
    } else {
        // Modello puro encoder (BERT, Nomic, etc.) -> USARE ENCODE
        if (llama_encode(engine->ctx, batch) != 0) {
            log_error("llama_encode fallito");
            free(tokens);
            return -1;
        }
    }

    const float *emb = llama_get_embeddings_seq(engine->ctx, 0);
    if (!emb) emb = llama_get_embeddings(engine->ctx); 

    if (!emb) {
        log_error("Embedding null");
        free(tokens);
        return -1;
    }

    float norm = 0.0f;
    for (int i = 0; i < engine->n_embd; i++) {
        norm += emb[i] * emb[i];
    }
    norm = sqrtf(norm);
    for (int i = 0; i < engine->n_embd; i++) {
        out_vector[i] = emb[i] / norm;
    }

    free(tokens);
    return 0;
}