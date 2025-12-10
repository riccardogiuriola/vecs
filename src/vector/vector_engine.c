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
#include <ctype.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#define EMBEDDING_PREFIX "Represent this sentence for searching relevant passages: "

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

    // --- PUNTO 2: Gestione Prefisso ---
    // Concateniamo PREFISSO + TESTO
    char *input_text = NULL;
    size_t prefix_len = strlen(EMBEDDING_PREFIX);
    size_t text_len = strlen(text);
    
    // Se il prefisso c'è, allochiamo nuova memoria
    if (prefix_len > 0) {
        input_text = malloc(prefix_len + text_len + 1);
        if (!input_text) return -1;
        strcpy(input_text, EMBEDDING_PREFIX);
        strcat(input_text, text);
    } else {
        // Altrimenti usiamo il puntatore diretto (cast per togliere const temporaneamente se serve, ma meglio strdup se tokenizzatore modifica)
        input_text = strdup(text); 
    }

    const struct llama_vocab *vocab = llama_model_get_vocab(engine->model);

    // Tokenizzazione
    int n_tokens_alloc = strlen(input_text) + 16; 
    llama_token *tokens = malloc(n_tokens_alloc * sizeof(llama_token));
    int n_tokens = llama_tokenize(vocab, input_text, strlen(input_text), tokens, n_tokens_alloc, true, false);

    // Gestione realloc token se buffer piccolo (come nel tuo codice originale) ...
    if (n_tokens < 0) {
        n_tokens_alloc = -n_tokens;
        tokens = realloc(tokens, n_tokens_alloc * sizeof(llama_token));
        n_tokens = llama_tokenize(vocab, input_text, strlen(input_text), tokens, n_tokens_alloc, true, false);
    }
    
    free(input_text);

    if (n_tokens <= 0) {
        free(tokens);
        return -1;
    }

    // Inferenza
    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    if (llama_decode(engine->ctx, batch) != 0) { // O llama_encode se modello solo encoder
        log_error("llama_decode fallito");
        free(tokens);
        return -1;
    }

    // --- PUNTO 1: Mean Pooling ---
    
    // Otteniamo il puntatore a TUTTI gli embedding generati (uno per token)
    // llama.cpp restituisce un array contiguo: [tok0_dim0...tok0_dimN, tok1_dim0...tok1_dimN, ...]
    float *all_embeddings = llama_get_embeddings(engine->ctx);
    
    if (!all_embeddings) {
        free(tokens);
        return -1;
    }

    // 1. Azzera output
    memset(out_vector, 0, engine->n_embd * sizeof(float));

    // 2. Somma vettoriale (Mean Pooling)
    for (int i = 0; i < n_tokens; i++) {
        // Puntatore all'embedding del token i-esimo
        float *token_emb = all_embeddings + (i * engine->n_embd);
        
        for (int j = 0; j < engine->n_embd; j++) {
            out_vector[j] += token_emb[j];
        }
    }

    // 3. Divisione per numero token
    for (int j = 0; j < engine->n_embd; j++) {
        out_vector[j] /= (float)n_tokens;
    }

    // 4. Normalizzazione L2 (Euclidea) finale
    // Essenziale per poter usare il Dot Product come Cosine Similarity
    float norm = 0.0f;
    for (int i = 0; i < engine->n_embd; i++) {
        norm += out_vector[i] * out_vector[i];
    }
    norm = sqrtf(norm);
    
    // Evita divisione per zero
    if (norm > 1e-9) { 
        for (int i = 0; i < engine->n_embd; i++) {
            out_vector[i] /= norm;
        }
    }

    free(tokens);
    return 0;
}