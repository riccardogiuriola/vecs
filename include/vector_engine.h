#ifndef VECS_VECTOR_ENGINE_H
#define VECS_VECTOR_ENGINE_H

#include <stddef.h>

typedef struct vector_engine_s vector_engine_t;

typedef enum {
    VECS_MODE_CPU,
    VECS_MODE_GPU
} vecs_execution_mode_t;

typedef struct {
    const char *model_path;
    int num_threads;         // Per CPU: numero contesti. Per GPU: ignorato (o usato per preprocessing).
    vecs_execution_mode_t mode; 
    int gpu_layers;          // 0 = CPU, 99 = Full GPU
} vecs_engine_config_t;

// Inizializza il modello (percorso file .gguf)
vector_engine_t *vector_engine_init(vecs_engine_config_t *config);

// Libera risorse
void vector_engine_destroy(vector_engine_t *engine);

// Genera embedding per una stringa
// out_vector deve essere allocato dal chiamante (dimensione dipende dal modello, es. 768 float)
int vector_engine_embed(vector_engine_t *engine, int thread_id, const char *text, float *out_vector);

// Calcola similarità coseno tra due vettori
float vector_math_cosine_similarity(const float *vec_a, const float *vec_b, size_t len);

// Dimensione del vettore (es. 384, 768, etc.)
int vector_engine_get_dim(vector_engine_t *engine);

#endif