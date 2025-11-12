/*
 * vecs Project: Header Server (Handle Opaco)
 * (include/server.h)
 */

#ifndef vecs_SERVER_H
#define vecs_SERVER_H

#include <stddef.h> // per size_t
#include <stdint.h> // <-- CORREZIONE: Aggiunto per int16_t e uint16_t

// --- Handle Opaco ---
// Il client (main.c) vede solo questo typedef e non
// conosce la struttura interna del server.
typedef struct vecs_server_ctx_s vecs_server_t;

// Forward declaration per la struct della connessione
struct vecs_connection_s;

/**
 * @brief Crea un nuovo contesto server e inizia l'ascolto sulla porta.
 * @param port La porta su cui ascoltare (es. "6379").
 * @return Un puntatore all'handle del server, o NULL in caso di fallimento.
 */
vecs_server_t* server_create(const char *port);

/**
 * @brief Avvia il loop eventi principale (Reactor).
 * Questa funzione blocca il thread (non ritorna mai).
 * @param server L'handle del server.
 */
void server_run(vecs_server_t *server);

/**
 * @brief Distrugge il server, chiude tutti i socket e libera le risorse.
 * @param server L'handle del server.
 */
void server_destroy(vecs_server_t *server);

/**
 * @brief (Interno) Rimuove una connessione dall'array di tracciamento del server.
 * Chiamato da connection_destroy.
 * @param server L'handle del server.
 * @param conn La connessione da rimuovere.
 */
void server_remove_connection(vecs_server_t *server, struct vecs_connection_s *conn);

/**
 * @brief (Interno) Registra o modifica un evento nel loop kqueue.
 * @param server L'handle del server.
 * @param fd Il file descriptor da monitorare.
 * @param filter (es. EVFILT_READ, EVFILT_WRITE).
 * @param flags (es. EV_ADD, EV_DELETE, EV_ENABLE).
 * @param udata Il puntatore utente da associare (es. la connessione).
 */
void server_register_event(vecs_server_t *server, int fd, int16_t filter, uint16_t flags, void *udata);


#endif // vecs_SERVER_H