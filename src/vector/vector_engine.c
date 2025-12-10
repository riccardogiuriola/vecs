/*
 * Vecs Project: Vector Engine (Wrapper Llama.cpp)
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

#ifndef EMBEDDING_PREFIX
#define EMBEDDING_PREFIX "Represent this sentence for searching relevant passages: "
#endif

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

    // --- 1. Preparazione Testo ---
    char *input_text = NULL;
    size_t prefix_len = strlen(EMBEDDING_PREFIX); 
    size_t text_len = strlen(text);
    
    if (prefix_len > 0) {
        input_text = malloc(prefix_len + text_len + 1);
        if (!input_text) return -1;
        memcpy(input_text, EMBEDDING_PREFIX, prefix_len);
        memcpy(input_text + prefix_len, text, text_len);
        input_text[prefix_len + text_len] = '\0'; 
    } else {
        input_text = strdup(text); 
    }

    if (!input_text) return -1;

    // --- 2. Tokenizzazione ---
    const struct llama_vocab *vocab = llama_model_get_vocab(engine->model);

    int n_tokens_alloc = prefix_len + text_len + 16; 
    llama_token *tokens = malloc(n_tokens_alloc * sizeof(llama_token));
    
    int n_tokens = llama_tokenize(vocab, input_text, strlen(input_text), tokens, n_tokens_alloc, true, false);

    if (n_tokens < 0) {
        n_tokens_alloc = -n_tokens;
        llama_token *new_tokens = realloc(tokens, n_tokens_alloc * sizeof(llama_token));
        if (!new_tokens) {
            free(tokens);
            free(input_text);
            return -1;
        }
        tokens = new_tokens;
        n_tokens = llama_tokenize(vocab, input_text, strlen(input_text), tokens, n_tokens_alloc, true, false);
    }
    
    free(input_text);

    if (n_tokens <= 0) {
        log_error("Errore tokenizzazione o stringa vuota");
        free(tokens);
        return -1;
    }

    // --- 3. Inferenza ---
    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    
    int ret = -1;

    // Controlliamo se il modello è Encoder-Only (es. BGE, BERT)
    if (llama_model_has_encoder(engine->model) && !llama_model_has_decoder(engine->model)) {
        ret = llama_encode(engine->ctx, batch);
    } else {
        ret = llama_decode(engine->ctx, batch);
    }

    if (ret != 0) {
        log_error("Inference fallita (ret: %d)", ret);
        free(tokens);
        return -1;
    }

    // --- 4. Mean Pooling ---
    float *all_embeddings = llama_get_embeddings_seq(engine->ctx, 0);
    if (!all_embeddings) {
        all_embeddings = llama_get_embeddings(engine->ctx);
    }

    if (!all_embeddings) {
        log_error("Impossibile ottenere embeddings da llama.cpp");
        free(tokens);
        return -1;
    }

    // Azzera output
    memset(out_vector, 0, engine->n_embd * sizeof(float));

    // Somma vettoriale (Mean Pooling)
    for (int i = 0; i < n_tokens; i++) {
        float *token_emb = all_embeddings + (i * engine->n_embd);
        for (int j = 0; j < engine->n_embd; j++) {
            out_vector[j] += token_emb[j];
        }
    }

    // Divisione per media
    for (int j = 0; j < engine->n_embd; j++) {
        out_vector[j] /= (float)n_tokens;
    }

    // Normalizzazione Euclidea
    float norm = 0.0f;
    for (int i = 0; i < engine->n_embd; i++) {
        norm += out_vector[i] * out_vector[i];
    }
    norm = sqrtf(norm);
    
    if (norm > 1e-9) { 
        for (int i = 0; i < engine->n_embd; i++) {
            out_vector[i] /= norm;
        }
    }

    free(tokens);
    return 0;
}