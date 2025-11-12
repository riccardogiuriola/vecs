/*
 * Vex Project: Implementazione Hash Map (Cache L1)
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

int hash_map_set(hash_map_t *map, const char *key, const char *value) {
    if (!map || !key || !value) return -1;

    uint64_t hash = hash_djb2(key);
    size_t index = hash % map->capacity;

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
            log_debug("Hash map: chiave '%s' aggiornata.", key);
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

    hm_node_t *node = map->buckets[index];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            // Trovato!
            return node->value;
        }
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