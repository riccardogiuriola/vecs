#ifndef VECS_L2_CACHE_H
#define VECS_L2_CACHE_H

#include <stddef.h>

typedef struct l2_cache_s l2_cache_t;

// Crea la cache L2
l2_cache_t *l2_cache_create(int vector_dim, size_t max_capacity);

// Distrugge la cache
void l2_cache_destroy(l2_cache_t *cache);

// Inserisce un embedding, IL PROMPT ORIGINALE, e la risposta
int l2_cache_insert(l2_cache_t *cache, const float *vector, const char *prompt_text, const char *response);

// Cerca il vettore più simile usando anche il testo per filtri ibridi
const char *l2_cache_search(l2_cache_t *cache, const float *query_vector, const char *query_text, float threshold);

// Rimuove un elemento semanticamente equivalente
int l2_cache_delete_semantic(l2_cache_t *cache, const float *query_vector);

#endif // VECS_L2_CACHE_H