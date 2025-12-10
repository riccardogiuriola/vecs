#ifndef VEX_L2_CACHE_H
#define VEX_L2_CACHE_H

#include <stddef.h>

typedef struct l2_cache_s l2_cache_t;

// Crea la cache L2 definendo la dimensione dei vettori (es. 768)
l2_cache_t *l2_cache_create(int vector_dim, size_t max_capacity);

void l2_cache_destroy(l2_cache_t *cache);

// Inserisce un embedding e la risposta associata
// key_text servirebbe per debug, vector è l'array di float, response la stringa
int l2_cache_insert(l2_cache_t *cache, const float *vector, const char *response);

// Cerca il vettore più simile.
// Restituisce la risposta se similarity > threshold, altrimenti NULL.
const char *l2_cache_search(l2_cache_t *cache, const float *query_vector, float threshold);

// Rimuove un elemento semanticamente equivalente al vettore dato.
// Ritorna 1 se cancellato, 0 se non trovato.
int l2_cache_delete_semantic(l2_cache_t* cache, const float* query_vector);

#endif