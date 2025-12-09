#include "l2_cache.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    float* vector;
    char* response;
} l2_entry_t;

struct l2_cache_s {
    l2_entry_t* entries;
    size_t size;
    size_t capacity;
    int vector_dim;
};

l2_cache_t* l2_cache_create(int vector_dim, size_t max_capacity) {
    l2_cache_t* cache = malloc(sizeof(l2_cache_t));
    cache->entries = calloc(max_capacity, sizeof(l2_entry_t));
    cache->size = 0;
    cache->capacity = max_capacity;
    cache->vector_dim = vector_dim;
    return cache;
}

void l2_cache_destroy(l2_cache_t* cache) {
    if (!cache) return;
    for (size_t i = 0; i < cache->size; i++) {
        free(cache->entries[i].vector);
        free(cache->entries[i].response);
    }
    free(cache->entries);
    free(cache);
}

int l2_cache_insert(l2_cache_t* cache, const float* vector, const char* response) {
    if (cache->size >= cache->capacity) {
        // MVP: Se piena, sovrascriviamo a caso o rifiutiamo. 
        // TODO: Implementare LRU eviction
        log_warn("L2 Cache piena! Impossibile inserire.");
        return -1;
    }

    l2_entry_t* entry = &cache->entries[cache->size];
    
    // Copia il vettore
    entry->vector = malloc(cache->vector_dim * sizeof(float));
    memcpy(entry->vector, vector, cache->vector_dim * sizeof(float));

    // Copia la risposta
    entry->response = strdup(response);

    cache->size++;
    return 0;
}

const char* l2_cache_search(l2_cache_t* cache, const float* query_vector, float threshold) {
    float max_score = -1.0f;
    int best_index = -1;

    // Linear Scan (OK per < 10k items su CPU moderne)
    // Poiché i vettori sono normalizzati (da vector_engine), 
    // Cosine Similarity == Dot Product.
    for (size_t i = 0; i < cache->size; i++) {
        float dot = 0.0f;
        float* v = cache->entries[i].vector;
        
        // Loop ottimizzabile con SIMD (AVX) in futuro
        for (int j = 0; j < cache->vector_dim; j++) {
            dot += query_vector[j] * v[j];
        }

        if (dot > max_score) {
            max_score = dot;
            best_index = i;
        }
    }

    if (best_index != -1 && max_score >= threshold) {
        log_info("HIT L2 (Score: %.4f) -> %.30s...", max_score, cache->entries[best_index].response);
        return cache->entries[best_index].response;
    }

    log_debug("MISS L2 (Best Score: %.4f)", max_score);
    return NULL;
}