/*
 * Vecs Project: IVFFlat Adaptive Cache (L2)
 * (src/cache/l2_cache.c)
 */

#include "l2_cache.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>
#include <float.h>

// --- COSTANTI DI TUNING ---
#define NUM_CLUSTERS 64      // Numero di "secchi"
#define N_PROBE 4            // Quanti secchi controllare durante la ricerca (Precisione vs Velocità)
#define ADAPT_RATE 0.1f      // Quanto velocemente i centroidi si adattano ai nuovi dati (0.1 = 10%)

// Struttura di una singola entry (come prima)
typedef struct {
    float* vector;
    char* original_prompt;
    char* response;
    time_t expire_at;
} l2_entry_t;

// Struttura del Cluster (Bucket)
typedef struct {
    float *centroid;         // Il vettore "media" di questo cluster
    l2_entry_t *entries;     // Array dinamico di entry
    size_t size;
    size_t capacity;
    int is_initialized;      // 0 se il centroide è vuoto/random, 1 se ha dati reali
} l2_cluster_t;

// Struttura principale Cache
struct l2_cache_s {
    l2_cluster_t clusters[NUM_CLUSTERS];
    int vector_dim;
    size_t total_count;      // Numero totale di elementi in tutti i cluster
    size_t max_global_capacity;
};

// --- HELPER MATH ---

// Prodotto scalare (Dot Product) ottimizzato
static float vec_dot(const float *a, const float *b, int dim) {
    float res = 0.0f;
    for (int i = 0; i < dim; i++) {
        res += a[i] * b[i];
    }
    return res;
}

// Aggiorna il centroide (Media mobile esponenziale semplificata)
// centroid = centroid * (1 - rate) + new_vec * rate
static void update_centroid(float *centroid, const float *new_vec, int dim) {
    for (int i = 0; i < dim; i++) {
        centroid[i] = centroid[i] * (1.0f - ADAPT_RATE) + new_vec[i] * ADAPT_RATE;
    }
    // Rinormalizzazione (importante per cosine similarity)
    float norm = 0.0f;
    for (int i = 0; i < dim; i++) norm += centroid[i] * centroid[i];
    norm = sqrtf(norm);
    if (norm > 1e-9) {
        for (int i = 0; i < dim; i++) centroid[i] /= norm;
    }
}

// --- API ---

l2_cache_t* l2_cache_create(int vector_dim, size_t max_capacity) {
    l2_cache_t* cache = calloc(1, sizeof(l2_cache_t));
    if (!cache) return NULL;

    cache->vector_dim = vector_dim;
    cache->max_global_capacity = max_capacity;
    cache->total_count = 0;

    // Inizializza i cluster
    for (int i = 0; i < NUM_CLUSTERS; i++) {
        cache->clusters[i].centroid = calloc(vector_dim, sizeof(float));
        // Capacità iniziale piccola per bucket (es. max / clusters * 1.5 buffer)
        size_t bucket_cap = (max_capacity / NUM_CLUSTERS) + 16;
        cache->clusters[i].entries = calloc(bucket_cap, sizeof(l2_entry_t));
        cache->clusters[i].capacity = bucket_cap;
        cache->clusters[i].size = 0;
        cache->clusters[i].is_initialized = 0;
    }

    log_info("L2 Cache IVFFlat creata: %d Clusters, Dim %d", NUM_CLUSTERS, vector_dim);
    return cache;
}

void l2_cache_destroy(l2_cache_t* cache) {
    if (!cache) return;
    for (int i = 0; i < NUM_CLUSTERS; i++) {
        for (size_t j = 0; j < cache->clusters[i].size; j++) {
            free(cache->clusters[i].entries[j].vector);
            free(cache->clusters[i].entries[j].original_prompt);
            free(cache->clusters[i].entries[j].response);
        }
        free(cache->clusters[i].entries);
        free(cache->clusters[i].centroid);
    }
    free(cache);
}

// Inserimento "Intelligente"
int l2_cache_insert(l2_cache_t* cache, const float* vector, const char* prompt_text, const char* response, int ttl_seconds) {
    if (cache->total_count >= cache->max_global_capacity) {
        // Policy semplificata: se pieno, rifiuta (per ora, o implementa LRU globale)
        // log_warn("L2 Cache Piena (Max: %zu)", cache->max_global_capacity);
        return -1; 
    }

    // 1. Trova il cluster migliore (Nearest Centroid)
    int best_cluster_idx = -1;
    float best_score = -2.0f; // Cosine va da -1 a 1

    // Se ci sono cluster non inizializzati, usiamoli per bootstrappare
    // Questo distribuisce i primi vettori uno per cluster.
    for (int i = 0; i < NUM_CLUSTERS; i++) {
        if (!cache->clusters[i].is_initialized) {
            best_cluster_idx = i;
            break;
        }
    }

    // Se tutti inizializzati, cerca il più simile
    if (best_cluster_idx == -1) {
        for (int i = 0; i < NUM_CLUSTERS; i++) {
            float score = vec_dot(cache->clusters[i].centroid, vector, cache->vector_dim);
            if (score > best_score) {
                best_score = score;
                best_cluster_idx = i;
            }
        }
    }

    l2_cluster_t *cluster = &cache->clusters[best_cluster_idx];

    // 2. Controllo spazio nel cluster (resize se necessario)
    if (cluster->size >= cluster->capacity) {
        size_t new_cap = cluster->capacity * 2;
        l2_entry_t *new_entries = realloc(cluster->entries, new_cap * sizeof(l2_entry_t));
        if (!new_entries) return -1;
        cluster->entries = new_entries;
        cluster->capacity = new_cap;
    }

    // 3. Inserimento effettivo
    l2_entry_t *entry = &cluster->entries[cluster->size];
    entry->vector = malloc(cache->vector_dim * sizeof(float));
    memcpy(entry->vector, vector, cache->vector_dim * sizeof(float));
    entry->original_prompt = strdup(prompt_text);
    entry->response = strdup(response);
    entry->expire_at = time(NULL) + ttl_seconds;

    cluster->size++;
    cache->total_count++;

    // 4. Aggiorna il centroide (Learning)
    if (!cluster->is_initialized) {
        // Primo elemento: il centroide diventa il vettore stesso
        memcpy(cluster->centroid, vector, cache->vector_dim * sizeof(float));
        cluster->is_initialized = 1;
    } else {
        // Elementi successivi: sposta il centroide verso il nuovo punto
        update_centroid(cluster->centroid, vector, cache->vector_dim);
    }

    return 0;
}

// Helper per gestire negazioni e lunghezza (dal codice precedente)
static int has_negation(const char* text) {
    char buffer[1024];
    strncpy(buffer, text, 1023);
    for(int i=0; buffer[i]; i++) buffer[i] = tolower(buffer[i]);
    if (strstr(buffer, " non ") || strstr(buffer, " no ") || strstr(buffer, " not ") || strstr(buffer, " mai ")) return 1;
    return 0;
}

// Struttura helper per ordinare i cluster durante la ricerca
typedef struct {
    int index;
    float score;
} cluster_score_t;

int compare_clusters(const void *a, const void *b) {
    float score_a = ((cluster_score_t*)a)->score;
    float score_b = ((cluster_score_t*)b)->score;
    // Ordinamento decrescente (score più alto prima)
    return (score_b > score_a) - (score_b < score_a);
}

const char* l2_cache_search(l2_cache_t* cache, const float* query_vector, const char* query_text, float threshold) {
    if (cache->total_count == 0) return NULL;

    // 1. Fase "Coarse Search": Trova i bucket candidati
    cluster_score_t candidates[NUM_CLUSTERS];
    int active_clusters = 0;

    for (int i = 0; i < NUM_CLUSTERS; i++) {
        if (cache->clusters[i].is_initialized && cache->clusters[i].size > 0) {
            candidates[active_clusters].index = i;
            candidates[active_clusters].score = vec_dot(cache->clusters[i].centroid, query_vector, cache->vector_dim);
            active_clusters++;
        }
    }

    if (active_clusters == 0) return NULL;

    // Ordina i cluster per somiglianza col centroide
    qsort(candidates, active_clusters, sizeof(cluster_score_t), compare_clusters);

    // 2. Fase "Fine Search": Cerca solo nei top N_PROBE cluster
    float max_score = -1.0f;
    int best_cluster_idx = -1;
    int best_entry_idx = -1;

    int probes = (active_clusters < N_PROBE) ? active_clusters : N_PROBE;
    
    // Prepariamo dati ausiliari query
    int query_has_neg = has_negation(query_text);
    size_t query_len = strlen(query_text);
    time_t now = time(NULL);

    for (int k = 0; k < probes; k++) {
        int c_idx = candidates[k].index;
        l2_cluster_t *cluster = &cache->clusters[c_idx];

        // Se lo score del centroide è troppo basso rispetto alla threshold, 
        // è inutile cercare dentro (Pruning euristico), a meno che non siamo disperati.
        // Ottimizzazione: se il centroide dista 0.5 e cerchiamo 0.9, è difficile trovare match dentro.
        // Ma per sicurezza controlliamo comunque i top probes.

        for (size_t i = 0; i < cluster->size; i++) {
            l2_entry_t *entry = &cluster->entries[i];

            // Lazy Deletion
            if (now > entry->expire_at) {
                // Swap with last
                free(entry->vector); free(entry->original_prompt); free(entry->response);
                cluster->entries[i] = cluster->entries[cluster->size - 1];
                cluster->size--;
                cache->total_count--;
                i--; 
                continue;
            }

            // Calcolo Score Vettoriale
            float dot = vec_dot(query_vector, entry->vector, cache->vector_dim);
            
            // Filtri Logici (Negazione / Lunghezza) - Penalità
            if (dot > 0.6f) {
                 size_t entry_len = strlen(entry->original_prompt);
                 long diff = (long)query_len - (long)entry_len;
                 if (diff < 0) diff = -diff;
                 float len_ratio = (float)diff / (float)(query_len > entry_len ? query_len : entry_len);
                 
                 if (len_ratio > 0.5f) dot *= 0.8f;

                 int entry_has_neg = has_negation(entry->original_prompt);
                 if (query_has_neg != entry_has_neg) dot *= 0.75f;
            }

            if (dot > max_score) {
                max_score = dot;
                best_cluster_idx = c_idx;
                best_entry_idx = i;
            }
        }
    }

    if (best_entry_idx != -1 && max_score >= threshold) {
        log_info("HIT L2 (IVF Score: %.4f) Cluster %d", max_score, best_cluster_idx);
        return cache->clusters[best_cluster_idx].entries[best_entry_idx].response;
    }

    return NULL;
}

// Cancellazione semantica (scan su nprobe cluster)
int l2_cache_delete_semantic(l2_cache_t* cache, const float* query_vector) {
    float threshold = 0.99f;
    
    // Logica duplicata dalla search ma per delete: cerchiamo nei cluster migliori
    cluster_score_t candidates[NUM_CLUSTERS];
    int active = 0;
    for(int i=0; i<NUM_CLUSTERS; i++) {
        if(cache->clusters[i].is_initialized) {
            candidates[active].index = i;
            candidates[active].score = vec_dot(cache->clusters[i].centroid, query_vector, cache->vector_dim);
            active++;
        }
    }
    qsort(candidates, active, sizeof(cluster_score_t), compare_clusters);
    
    int probes = (active < N_PROBE) ? active : N_PROBE;
    
    for(int k=0; k<probes; k++) {
        l2_cluster_t *c = &cache->clusters[candidates[k].index];
        for(size_t i=0; i<c->size; i++) {
            float dot = vec_dot(query_vector, c->entries[i].vector, cache->vector_dim);
            if(dot >= threshold) {
                // Delete
                free(c->entries[i].vector);
                free(c->entries[i].original_prompt);
                free(c->entries[i].response);
                c->entries[i] = c->entries[c->size-1];
                c->size--;
                cache->total_count--;
                log_info("L2 Semantic Delete OK.");
                return 1;
            }
        }
    }
    return 0;
}

// Clear completo
void l2_cache_clear(l2_cache_t *cache) {
    if (!cache) return;
    for (int i = 0; i < NUM_CLUSTERS; i++) {
        for (size_t j = 0; j < cache->clusters[i].size; j++) {
            free(cache->clusters[i].entries[j].vector);
            free(cache->clusters[i].entries[j].original_prompt);
            free(cache->clusters[i].entries[j].response);
        }
        cache->clusters[i].size = 0;
        cache->clusters[i].is_initialized = 0; 
        // Nota: non liberiamo i centroidi qui, li resettiamo logicamente
        memset(cache->clusters[i].centroid, 0, cache->vector_dim * sizeof(float));
    }
    cache->total_count = 0;
    log_debug("L2 Cache (IVF) svuotata.");
}

// Helper per il caricamento/salvataggio (raw insert specifico per il clustering)
int l2_cache_insert_raw(l2_cache_t *cache, float *vector, const char *prompt, const char *resp, time_t expire_at) {
    // Riutilizziamo la logica di insert normale per assegnare il cluster corretto
    // anche durante il caricamento da disco. Questo "ri-addestra" i centroidi al boot.
    return l2_cache_insert(cache, vector, prompt, resp, (int)(expire_at - time(NULL)));
}

// SAVE: Salva come stream piatto (il formato su disco non cambia!)
int l2_cache_save(l2_cache_t *cache, FILE *f) {
    if (!cache || !f) return -1;
    uint8_t section_id = 0x02;
    fwrite(&section_id, sizeof(uint8_t), 1, f);
    fwrite(&cache->vector_dim, sizeof(int), 1, f);

    int count = 0;
    time_t now = time(NULL);

    // Itera su tutti i cluster e salva linearmente
    for (int i = 0; i < NUM_CLUSTERS; i++) {
        l2_cluster_t *c = &cache->clusters[i];
        for (size_t j = 0; j < c->size; j++) {
            l2_entry_t *e = &c->entries[j];
            if (e->expire_at > now) {
                uint8_t valid = 1;
                fwrite(&valid, sizeof(uint8_t), 1, f);
                fwrite(e->vector, sizeof(float), cache->vector_dim, f);
                
                int p_len = strlen(e->original_prompt);
                fwrite(&p_len, sizeof(int), 1, f);
                fwrite(e->original_prompt, sizeof(char), p_len, f);

                int r_len = strlen(e->response);
                fwrite(&r_len, sizeof(int), 1, f);
                fwrite(e->response, sizeof(char), r_len, f);

                fwrite(&e->expire_at, sizeof(time_t), 1, f);
                count++;
            }
        }
    }
    uint8_t end_marker = 0;
    fwrite(&end_marker, sizeof(uint8_t), 1, f);
    log_info("L2 Cache salvata (IVF Flat): %d vettori totali.", count);
    return 0;
}

// LOAD: Carica e reinserisce (ricostruendo i cluster)
int l2_cache_load(l2_cache_t *cache, FILE *f) {
    // La logica di load in l2_cache.h può rimanere la stessa se chiama l2_cache_insert_raw
    // Siccome ho ridefinito l2_cache_insert_raw qui sopra per usare l'inserimento clusterizzato,
    // possiamo copiare il corpo della funzione load dal vecchio file o includerla qui.
    
    // COPIA-INCOLLA la funzione load dal vecchio file, 
    // assicurandoti che chiami la NUOVA l2_cache_insert_raw definita qui sopra.
    
    uint8_t section_id;
    if (fread(&section_id, sizeof(uint8_t), 1, f) != 1 || section_id != 0x02) {
        log_error("L2 Load: Section ID mismatch"); return -1;
    }
    int dim_check;
    fread(&dim_check, sizeof(int), 1, f);
    if (dim_check != cache->vector_dim) return -1;

    int loaded = 0;
    time_t now = time(NULL);
    float *tmp_vec = malloc(cache->vector_dim * sizeof(float));

    while (1) {
        uint8_t valid;
        if (fread(&valid, sizeof(uint8_t), 1, f) != 1) break;
        if (valid == 0) break;

        fread(tmp_vec, sizeof(float), cache->vector_dim, f);

        int p_len; fread(&p_len, sizeof(int), 1, f);
        char *prompt = malloc(p_len + 1);
        fread(prompt, sizeof(char), p_len, f);
        prompt[p_len] = '\0';

        int r_len; fread(&r_len, sizeof(int), 1, f);
        char *resp = malloc(r_len + 1);
        fread(resp, sizeof(char), r_len, f);
        resp[r_len] = '\0';

        time_t expire_at;
        fread(&expire_at, sizeof(time_t), 1, f);

        if (expire_at > now) {
            // Qui avviene la magia: ricalcola i cluster mentre carica!
            l2_cache_insert(cache, tmp_vec, prompt, resp, (int)(expire_at - now));
            loaded++;
        }
        free(prompt); free(resp);
    }
    free(tmp_vec);
    log_info("L2 Cache caricata e re-indicizzata: %d vettori.", loaded);
    return 0;
}