#include "l2_cache.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

typedef struct {
    float* vector;
    char* original_prompt;
    char* response;
    time_t expire_at;
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
        free(cache->entries[i].original_prompt);
    }
    free(cache->entries);
    free(cache);
}

int l2_cache_insert(l2_cache_t* cache, const float* vector, const char* prompt_text, const char* response, int ttl_seconds) {
    if (cache->size >= cache->capacity) {
        log_warn("L2 Cache piena!");
        return -1;
    }

    l2_entry_t* entry = &cache->entries[cache->size];
    
    entry->vector = malloc(cache->vector_dim * sizeof(float));
    memcpy(entry->vector, vector, cache->vector_dim * sizeof(float));

    entry->original_prompt = strdup(prompt_text);
    entry->response = strdup(response);

    entry->expire_at = time(NULL) + ttl_seconds;

    cache->size++;
    return 0;
}

static int has_negation(const char* text) {
    // Controllo molto semplice (case insensitive substring)
    // Per produzione servirebbe tokenizzazione accurata
    char buffer[1024];
    strncpy(buffer, text, 1023);
    for(int i=0; buffer[i]; i++) buffer[i] = tolower(buffer[i]);

    if (strstr(buffer, " non ") || strstr(buffer, " no ") || strstr(buffer, " not ") || 
        strstr(buffer, " never ") || strstr(buffer, " mai ")) {
        return 1;
    }
    return 0;
}

const char* l2_cache_search(l2_cache_t* cache, const float* query_vector, const char* query_text, float threshold) {
    float max_score = -1.0f;
    int best_index = -1;
    
    // Pre-calcolo negazione query per velocità
    int query_has_neg = has_negation(query_text);
    size_t query_len = strlen(query_text);
    time_t now = time(NULL);

    for (size_t i = 0; i < cache->size; i++) {

        // 1. CHECK SCADENZA (Lazy Deletion)
        if (now > cache->entries[i].expire_at) {
            // Elemento scaduto: logga e rimuovi
            // log_debug("L2 EXPIRED: Rimuovo vettore scaduto all'indice %zu", i);
            
            // Libera memoria dell'elemento corrente
            free(cache->entries[i].vector);
            free(cache->entries[i].original_prompt);
            free(cache->entries[i].response);

            // Sposta l'ultimo elemento qui (Swap with Last)
            if (i != cache->size - 1) {
                cache->entries[i] = cache->entries[cache->size - 1];
            }
            
            cache->size--;
            i--; // Decrementa i per ricontrollare questo indice (ora contiene il vecchio ultimo elemento)
            continue; 
        }

        float dot = 0.0f;
        float* v = cache->entries[i].vector;
        
        // 1. Vector Score (Cosine Similarity)
        for (int j = 0; j < cache->vector_dim; j++) {
            dot += query_vector[j] * v[j];
        }
        
        float final_score = dot;

        // Se il punteggio vettoriale è decente, applichiamo i filtri ibridi
        // per vedere se dobbiamo PENALIZZARLO.
        if (final_score > 0.5f) { // Optimization: non sprecare CPU su match scarsi
            
            l2_entry_t* entry = &cache->entries[i];

            // --- PUNTO 5: Length Filtering ---
            // Se le lunghezze differiscono drasticamente (> 50%), probabilmente è sbagliato
            size_t entry_len = strlen(entry->original_prompt);
            long diff = (long)query_len - (long)entry_len;
            if (diff < 0) diff = -diff; // abs
            
            float len_ratio = (float)diff / (float)(query_len > entry_len ? query_len : entry_len);
            
            if (len_ratio > 0.5f) {
                // Penalità drastica: riduci lo score del 20%
                final_score *= 0.8f;
                // log_debug("Penalità lunghezza applicata: %.2f", len_ratio);
            }

            // --- PUNTO 4: Negation Mismatch ---
            // Se uno ha "non" e l'altro no, è gravissimo.
            int entry_has_neg = has_negation(entry->original_prompt);
            if (query_has_neg != entry_has_neg) {
                // Penalità severa: riduci lo score del 25%
                // Questo trasforma un match 0.90 (alto) in 0.67 (spesso sotto soglia)
                final_score *= 0.75f;
                // log_debug("Penalità negazione applicata");
            }
        }

        if (final_score > max_score) {
            max_score = final_score;
            best_index = (int)i;
        }
    }

    if (best_index != -1 && max_score >= threshold) {
        log_info("HIT L2 (Final Score: %.4f) -> %.30s...", max_score, cache->entries[best_index].response);
        return cache->entries[best_index].response;
    }

    log_debug("MISS L2 (Best Score: %.4f)", max_score);
    return NULL;
}

int l2_cache_delete_semantic(l2_cache_t* cache, const float* query_vector) {
    // Soglia altissima per la cancellazione: vogliamo cancellare solo se è praticamente lo stesso concetto
    float delete_threshold = 0.99f; 
    
    int best_index = -1;
    float max_score = -1.0f;

    // 1. Trova l'elemento
    for (size_t i = 0; i < cache->size; i++) {
        float dot = 0.0f;
        float* v = cache->entries[i].vector;
        for (int j = 0; j < cache->vector_dim; j++) {
            dot += query_vector[j] * v[j];
        }

        if (dot > max_score) {
            max_score = dot;
            best_index = i;
        }
    }

    // 2. Se trovato e supera la soglia, cancella
    if (best_index != -1 && max_score >= delete_threshold) {
        // Libera memoria
        free(cache->entries[best_index].vector);
        free(cache->entries[best_index].response);

        // Ottimizzazione "Swap with Last":
        // Invece di spostare tutto l'array (lento), prendiamo l'ultimo elemento
        // e lo mettiamo nel buco lasciato da quello cancellato.
        // L'ordine non conta nella ricerca vettoriale.
        if (best_index < (int)cache->size - 1) {
            cache->entries[best_index] = cache->entries[cache->size - 1];
        }

        cache->size--;
        log_info("L2 DELETE: Rimosso elemento (Score: %.4f)", max_score);
        return 1; // Cancellato
    }

    return 0; // Non trovato
}