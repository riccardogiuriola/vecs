#ifndef VEX_VECTOR_ENGINE_H
#define VEX_VECTOR_ENGINE_H

#include <stddef.h>

typedef struct vector_engine_s vector_engine_t;

// Inizializza il modello (percorso file .gguf)
vector_engine_t *vector_engine_init(const char *model_path);

// Libera risorse
void vector_engine_destroy(vector_engine_t *engine);

// Genera embedding per una stringa
// out_vector deve essere allocato dal chiamante (dimensione dipende dal modello, es. 768 float)
int vector_engine_embed(vector_engine_t *engine, const char *text, float *out_vector);

// Calcola similarità coseno tra due vettori
float vector_math_cosine_similarity(const float *vec_a, const float *vec_b, size_t len);

// Dimensione del vettore (es. 384, 768, etc.)
int vector_engine_get_dim(vector_engine_t *engine);

#endif