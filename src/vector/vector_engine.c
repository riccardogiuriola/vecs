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

struct vector_engine_s {
    struct llama_model *model;
    struct llama_context *ctx;
    int n_embd;
    struct llama_batch batch; 
    int batch_capacity;
    int is_bert; // Flag per architettura encoder-only
};

vector_engine_t* vector_engine_init(const char *model_path) {
    llama_backend_init();

    struct llama_model_params model_params = llama_model_default_params();
    // Ottimizzazione per Apple Silicon (M1/M2/M3)
    model_params.n_gpu_layers = 99; 

    // FIX WARNING: llama_load_model_from_file -> llama_model_load_from_file
    struct llama_model *model = llama_model_load_from_file(model_path, model_params);
    if (!model) {
        log_error("Fallito caricamento modello: %s", model_path);
        return NULL;
    }

    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 2048; 
    ctx_params.embeddings = true; // Necessario per estrarre vettori

    // FIX WARNING: llama_new_context_with_model -> llama_init_from_model
    struct llama_context *ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) {
        log_error("Fallita creazione contesto llama");
        // FIX WARNING: llama_free_model -> llama_model_free
        llama_model_free(model);
        return NULL;
    }

    vector_engine_t *engine = malloc(sizeof(vector_engine_t));
    engine->model = model;
    engine->ctx = ctx;
    
    // FIX WARNING: llama_n_embd -> llama_model_n_embd
    engine->n_embd = llama_model_n_embd(model); 
    
    engine->batch_capacity = 2048;
    engine->batch = llama_batch_init(engine->batch_capacity, 0, 1);

    // --- Rilevamento Architettura ---
    engine->is_bert = 0;
    if (llama_model_has_encoder(model)) {
        engine->is_bert = 1;
    } else {
        // Fallback: controllo stringa architettura
        char arch[128] = {0};
        if (llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch)) > 0) {
            if (strstr(arch, "bert") || strstr(arch, "nomic-bert") || strstr(arch, "roberta")) {
                engine->is_bert = 1;
            }
        }
    }

    log_info("Vector Engine Init: Dim=%d, Mode=%s", 
             engine->n_embd, engine->is_bert ? "Encoder (BERT/BGE)" : "Decoder (Llama/GPT)");
    
    return engine;
}

void vector_engine_destroy(vector_engine_t *engine) {
    if (!engine) return;
    llama_batch_free(engine->batch);
    if (engine->ctx) llama_free(engine->ctx);
    // FIX WARNING: llama_free_model -> llama_model_free
    if (engine->model) llama_model_free(engine->model);
    free(engine);
    llama_backend_free();
}

int vector_engine_get_dim(vector_engine_t *engine) {
    return engine->n_embd;
}

int vector_engine_embed(vector_engine_t *engine, const char *text, float *out_vector) {
    if (!engine || !text || !out_vector) return -1;

    // --- 1. Tokenizzazione ---
    const struct llama_vocab *vocab = llama_model_get_vocab(engine->model);
    int n_tokens_alloc = strlen(text) + 2; 
    llama_token *tokens = malloc(n_tokens_alloc * sizeof(llama_token));
    
    // add_special = true è CRUCIALE per BGE/BERT (aggiunge il token CLS all'inizio)
    int n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_tokens_alloc, true, false);

    if (n_tokens < 0) {
        n_tokens_alloc = -n_tokens;
        tokens = realloc(tokens, n_tokens_alloc * sizeof(llama_token));
        n_tokens = llama_tokenize(vocab, text, strlen(text), tokens, n_tokens_alloc, true, false);
    }

    if (n_tokens <= 0) {
        free(tokens);
        return -1;
    }

    if (n_tokens > engine->batch_capacity) n_tokens = engine->batch_capacity;

    // --- 2. Preparazione Batch ---
    engine->batch.n_tokens = n_tokens;
    for (int i = 0; i < n_tokens; i++) {
        engine->batch.token[i] = tokens[i];
        engine->batch.pos[i] = i;
        engine->batch.seq_id[i][0] = 0; // Sequenza 0
        engine->batch.n_seq_id[i] = 1;
        engine->batch.logits[i] = true; // Richiedi output per tutti (serve per il pooling)
    }

    // --- 3. Inferenza ---
    
    // Pulizia memoria KV (Nuova API llama.cpp)
    llama_memory_t mem = llama_get_memory(engine->ctx);
    llama_memory_seq_rm(mem, -1, -1, -1);

    int ret = 0;
    if (engine->is_bert) {
        ret = llama_encode(engine->ctx, engine->batch);
    } else {
        ret = llama_decode(engine->ctx, engine->batch);
    }

    if (ret != 0) {
        log_error("Inference fallita: %d", ret);
        free(tokens);
        return -1;
    }

    // --- 4. Estrazione Embedding (Auto-Pooling) ---
    
    // Proviamo a usare la funzione di alto livello che rispetta il pooling_type del modello (CLS vs Mean)
    float *emb = llama_get_embeddings_seq(engine->ctx, 0);
    
    if (emb) {
        // Copia diretta dal buffer gestito da llama.cpp
        memcpy(out_vector, emb, engine->n_embd * sizeof(float));
    } else {
        // Fallback Manuale (Se il pooling non è definito nel modello)
        if (engine->is_bert) {
            // CLS Pooling (Token 0)
            float *cls = llama_get_embeddings_ith(engine->ctx, 0);
            if (cls) memcpy(out_vector, cls, engine->n_embd * sizeof(float));
        } else {
            // Mean Pooling
            memset(out_vector, 0, engine->n_embd * sizeof(float));
            int count = 0;
            for (int i = 0; i < n_tokens; i++) {
                float *t = llama_get_embeddings_ith(engine->ctx, i);
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

    // --- 5. Normalizzazione Euclidea ---
    float norm = 0.0f;
    for (int i = 0; i < engine->n_embd; i++) norm += out_vector[i] * out_vector[i];
    norm = sqrtf(norm);
    if (norm > 1e-9) {
        for (int i = 0; i < engine->n_embd; i++) out_vector[i] /= norm;
    }

    free(tokens);
    return 0;
}