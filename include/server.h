/*
 * Vex Project: Header Server (Handle Opaco)
 * (include/server.h)
 */
#ifndef VEX_SERVER_H
#define VEX_SERVER_H

#include <stdint.h> // Per tipi a larghezza fissa (usati da kqueue/epoll)

/*
 * Handle opachi per le strutture principali.
 * Le definizioni complete si trovano nei rispettivi file .c
 * Questo previene l'inquinamento dello scope e impone l'incapsulamento.
 */
typedef struct vex_server_s vex_server_t;
typedef struct vex_connection_s vex_connection_t;
typedef struct event_loop_s event_loop_t;
typedef struct hash_map_s hash_map_t; // <-- AGGIUNTO


/**
 * @brief Crea una nuova istanza del server.
 * * @param port La porta su cui mettersi in ascolto (come stringa, es. "6379").
 * @return Un puntatore al nuovo vex_server_t o NULL in caso di errore.
 */
vex_server_t* server_create(const char *port); // <-- CORREZIONE: da int a const char*

/**
 * @brief Avvia il loop eventi principale del server.
 * Questa funzione blocca il thread corrente e gestisce l'I/O.
 * * @param server Il server da avviare.
 * @return 0 in caso di uscita normale, -1 in caso di errore critico.
 */
int server_run(vex_server_t *server);

/**
 * @brief Distrugge il server e libera tutte le risorse.
 * * @param server Il server da distruggere.
 */
void server_destroy(vex_server_t *server);

/**
 * @brief Aggiunge una nuova connessione client al server.
 * (Questa funzione è "interna" al core, ma definita qui
 * perché server.c ha bisogno di connection.h e viceversa)
 * * @param server Il server.
 * @param client_fd Il file descriptor del nuovo client.
 * @return Il puntatore alla nuova connessione, o NULL.
 */
vex_connection_t* server_add_connection(vex_server_t *server, int client_fd);

/**
 * @brief Rimuove e distrugge una connessione client.
 * * @param conn La connessione da rimuovere.
 */
void server_remove_connection(vex_connection_t *conn);

/**
 * @brief Ottiene l'event loop associato al server.
 * Usato da connection.c per registrare/deregistrare eventi.
 * * @param server Il server.
 * @return Il puntatore all'event_loop_t.
 */
event_loop_t* server_get_loop(vex_server_t *server);

/**
 * @brief Ottiene la cache L1 (hash map) associata al server.
 * * @param server Il server.
 * @return Il puntatore alla hash_map_t.
 */
hash_map_t* server_get_l1_cache(vex_server_t *server); // <-- AGGIUNTO


#endif // VEX_SERVER_H