/*
 * Vecs Project: Header Hash Map (Cache L1)
 * (include/hash_map.h)
 */
#ifndef VECS_HASH_MAP_H
#define VECS_HASH_MAP_H

#include <stddef.h> // Per size_t
#include <time.h>   // Per time_t
#include <stdio.h>

/*
 * Struttura di un nodo nella hash map (gestione collisioni con linked list)
 * Questa struttura è interna (opaca) al .c
 */
typedef struct hm_node_s hm_node_t;

/*
 * L'handle opaco per la nostra hash map.
 * La definizione completa si trova in hash_map.c
 */
typedef struct hash_map_s hash_map_t;

/**
 * @brief Crea una nuova hash map.
 * * @param initial_capacity La capacità iniziale (numero di bucket).
 * Una potenza di 2 è raccomandata per performance migliori.
 * @return Un puntatore alla nuova hash_map_t o NULL in caso di errore.
 */
hash_map_t *hash_map_create(size_t initial_capacity);

/**
 * @brief Distrugge una hash map e libera tutta la memoria.
 * * @param map La mappa da distruggere.
 */
void hash_map_destroy(hash_map_t *map);

/**
 * @brief Inserisce o aggiorna una coppia chiave-valore nella mappa.
 * La funzione crea copie interne sia della chiave che del valore.
 * * @param map La mappa.
 * @param key La chiave (stringa C).
 * @param value Il valore (stringa C).
 * @return 0 in caso di successo, -1 in caso di errore (es. allocazione memoria).
 */
int hash_map_set(hash_map_t *map, const char *key, const char *value, int ttl_seconds);

/**
 * @brief Recupera un valore dalla mappa usando la chiave.
 * * @param map La mappa.
 * @param key La chiave da cercare.
 * @return Un puntatore costante al valore *interno* se trovato, altrimenti NULL.
 * NON modificare o liberare questo puntatore.
 */
const char *hash_map_get(hash_map_t *map, const char *key);

/**
 * @brief Rimuove una coppia chiave-valore dalla mappa.
 * (Non usato nell'MVP ma utile per la gestione della cache)
 * * @param map La mappa.
 * @param key La chiave da rimuovere.
 */
void hash_map_delete(hash_map_t *map, const char *key);

/**
 * @brief Svuota la cache.
 * * @param map La mappa.
 */
void hash_map_clear(hash_map_t *map);

// Salva tutto il contenuto su un file aperto
int hash_map_save(hash_map_t *map, FILE *f);

// Carica contenuto da file (ignora chiavi scadute)
int hash_map_load(hash_map_t *map, FILE *f);

#endif // VECS_HASH_MAP_H