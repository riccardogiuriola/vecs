/*
 * Vecs Project: Implementazione Hash Map (Cache L1)
 * (src/cache/hash_map.c)
 * * Implementazione semplice con separate chaining.
 */

#include "hash_map.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h> // Per uint64_t

// --- Definizione Strutture Interne ---

/**
 * @brief Nodo della linked list per la gestione delle collisioni.
 */
struct hm_node_s {
    char *key;
    char *value;
    time_t expire_at;
    struct hm_node_s *next;
};

/**
 * @brief Struttura principale della Hash Map.
 */
struct hash_map_s {
    size_t capacity;
    size_t size;
    hm_node_t **buckets; // Array di puntatori a nodi
};


// --- Funzione di Hashing ---

/**
 * @brief Algoritmo di Hashing djb2 (variante a 64 bit).
 * Molto veloce e con buona distribuzione per stringhe.
 */
static uint64_t hash_djb2(const char *str) {
    uint64_t hash = 5381;
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}


// --- Funzioni Helper Interne ---

/**
 * @brief Libera la memoria di un singolo nodo.
 */
static void hm_node_destroy(hm_node_t *node) {
    if (!node) return;
    free(node->key);
    free(node->value);
    free(node);
}


// --- Implementazione API Pubbliche ---

hash_map_t* hash_map_create(size_t initial_capacity) {
    if (initial_capacity == 0) {
        initial_capacity = 1024; // Default
    }

    hash_map_t *map = calloc(1, sizeof(hash_map_t));
    if (!map) {
        log_error("hash_map_create: Impossibile allocare memoria per la mappa.");
        return NULL;
    }

    map->capacity = initial_capacity;
    map->size = 0;
    
    // usiamo calloc per inizializzare tutti i bucket a NULL
    map->buckets = calloc(map->capacity, sizeof(hm_node_t*));
    if (!map->buckets) {
        log_error("hash_map_create: Impossibile allocare memoria per i bucket.");
        free(map);
        return NULL;
    }

    log_debug("Hash map creata con capacità %zu", initial_capacity);
    return map;
}

void hash_map_destroy(hash_map_t *map) {
    if (!map) return;

    for (size_t i = 0; i < map->capacity; i++) {
        hm_node_t *node = map->buckets[i];
        while (node) {
            hm_node_t *next = node->next;
            hm_node_destroy(node);
            node = next;
        }
    }
    
    free(map->buckets);
    free(map);
    log_debug("Hash map distrutta.");
}

int hash_map_set(hash_map_t *map, const char *key, const char *value, int ttl_seconds) {
    if (!map || !key || !value) return -1;

    uint64_t hash = hash_djb2(key);
    size_t index = hash % map->capacity;

    time_t now = time(NULL);
    time_t expire_at = now + ttl_seconds;

    hm_node_t *node = map->buckets[index];
    hm_node_t *prev = NULL;

    // 1. Cerca se la chiave esiste già (e aggiorna)
    while (node) {
        if (strcmp(node->key, key) == 0) {
            // Trovato! Aggiorna il valore in-place.
            char *new_value = strdup(value);
            if (!new_value) {
                log_warn("hash_map_set: fallita allocazione per aggiornamento valore.");
                return -1;
            }
            free(node->value);
            node->value = new_value;
            node->expire_at = expire_at;
            log_debug("L1 SET: Chiave '%s' aggiornata (TTL: %ds)", key, ttl_seconds);
            return 0;
        }
        prev = node;
        node = node->next;
    }

    // 2. Chiave non trovata, crea un nuovo nodo
    hm_node_t *new_node = malloc(sizeof(hm_node_t));
    if (!new_node) {
         log_warn("hash_map_set: fallita allocazione per nuovo nodo.");
         return -1;
    }

    new_node->key = strdup(key);
    new_node->value = strdup(value);
    new_node->expire_at = expire_at;
    new_node->next = NULL;

    if (!new_node->key || !new_node->value) {
        log_warn("hash_map_set: fallita allocazione per chiave/valore.");
        hm_node_destroy(new_node); // Libera tutto
        return -1;
    }
    
    // 3. Aggiungi il nodo alla lista (head o tail)
    if (prev == NULL) {
        // È il primo nodo in questo bucket
        map->buckets[index] = new_node;
    } else {
        // Aggiungi in coda alla lista
        prev->next = new_node;
    }

    map->size++;
    log_debug("Hash map: chiave '%s' inserita.", key);

    // TODO: Aggiungere logica di resize (rehashing) se map->size / map->capacity > 0.75
    
    return 0;
}

const char* hash_map_get(hash_map_t *map, const char *key) {
    if (!map || !key) return NULL;

    uint64_t hash = hash_djb2(key);
    size_t index = hash % map->capacity;
    time_t now = time(NULL);

    hm_node_t *node = map->buckets[index];
    hm_node_t *prev = NULL;
    while (node) {
        if (strcmp(node->key, key) == 0) {

            log_debug("CHECK KEY: '%s' | Now: %ld | ExpireAt: %ld | Diff: %ld", 
                      key, (long)now, (long)node->expire_at, (long)(node->expire_at - now));

            if (now > node->expire_at) {
                log_info("L1 EXPIRED: Chiave '%s' scaduta. Rimozione lazy.", key);
                
                // Rimuovi nodo dalla lista
                if (prev == NULL) map->buckets[index] = node->next;
                else prev->next = node->next;

                hm_node_destroy(node);
                map->size--;
                return NULL; // Tratta come MISS
            }
            // Trovato!
            return node->value;
        }
        prev = node;
        node = node->next;
    }

    // Non trovato
    return NULL;
}

void hash_map_delete(hash_map_t *map, const char *key) {
    if (!map || !key) return;

    uint64_t hash = hash_djb2(key);
    size_t index = hash % map->capacity;

    hm_node_t *node = map->buckets[index];
    hm_node_t *prev = NULL;

    while (node) {
        if (strcmp(node->key, key) == 0) {
            // Trovato, rimuovi
            if (prev == NULL) {
                // Era il nodo di testa
                map->buckets[index] = node->next;
            } else {
                // Era in mezzo o in coda
                prev->next = node->next;
            }
            hm_node_destroy(node);
            map->size--;
            log_debug("Hash map: chiave '%s' rimossa.", key);
            return;
        }
        prev = node;
        node = node->next;
    }
    // Chiave non trovata, non fa nulla
}

void hash_map_clear(hash_map_t *map) {
    if (!map) return;

    for (size_t i = 0; i < map->capacity; i++) {
        hm_node_t *node = map->buckets[i];
        while (node) {
            hm_node_t *next = node->next;
            hm_node_destroy(node);
            node = next;
        }
        map->buckets[i] = NULL;
    }
    map->size = 0;
    log_debug("L1 Cache svuotata.");
}

int hash_map_save(hash_map_t *map, FILE *f) {
    if (!map || !f) return -1;
    
    int count = 0;
    time_t now = time(NULL);

    // Scriviamo un header per la sezione L1 (numero di elementi stimato o placeholder)
    // Per semplicità, iteriamo e scriviamo sequenzialmente.
    
    // Marcatore inizio sezione L1
    uint8_t section_id = 0x01; 
    fwrite(&section_id, sizeof(uint8_t), 1, f);

    for (size_t i = 0; i < map->capacity; i++) {
        hm_node_t *node = map->buckets[i];
        while (node) {
            // Salva solo se non è già scaduto
            if (node->expire_at > now) {
                int key_len = strlen(node->key);
                int val_len = strlen(node->value);

                fwrite(&key_len, sizeof(int), 1, f);
                fwrite(node->key, sizeof(char), key_len, f);
                fwrite(&val_len, sizeof(int), 1, f);
                fwrite(node->value, sizeof(char), val_len, f);
                fwrite(&node->expire_at, sizeof(time_t), 1, f);
                count++;
            }
            node = node->next;
        }
    }
    
    // Marcatore fine sezione (key_len -1 o simile, o semplicemente gestito dal caller)
    // Usiamo un key_len speciale 0 per dire "fine lista"
    int end_marker = 0;
    fwrite(&end_marker, sizeof(int), 1, f);
    
    log_info("Hash Map salvata: %d chiavi.", count);
    return 0;
}

int hash_map_load(hash_map_t *map, FILE *f) {
    uint8_t section_id;
    if (fread(&section_id, sizeof(uint8_t), 1, f) != 1 || section_id != 0x01) {
        log_error("Formato file corrotto (L1 header missing)");
        return -1;
    }

    int loaded_count = 0;
    time_t now = time(NULL);

    while (1) {
        int key_len;
        if (fread(&key_len, sizeof(int), 1, f) != 1) break; // EOF o errore
        
        if (key_len == 0) break; // Fine sezione

        char *key = malloc(key_len + 1);
        fread(key, sizeof(char), key_len, f);
        key[key_len] = '\0';

        int val_len;
        fread(&val_len, sizeof(int), 1, f);
        char *val = malloc(val_len + 1);
        fread(val, sizeof(char), val_len, f);
        val[val_len] = '\0';

        time_t expire_at;
        fread(&expire_at, sizeof(time_t), 1, f);

        // Controllo TTL al caricamento
        if (expire_at > now) {
            // Calcoliamo il TTL rimanente
            int ttl = (int)(expire_at - now);
            hash_map_set(map, key, val, ttl);
            loaded_count++;
        }

        free(key);
        free(val);
    }
    
    log_info("Hash Map caricata: %d chiavi.", loaded_count);
    return 0;
}