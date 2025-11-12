/*
 * Vex Project: Header Server (Handle Opaco)
 * (include/server.h)
 *
 * MODIFICATO per Fase 6 (Astrazione Event Loop)
 */

#ifndef VEX_SERVER_H
#define VEX_SERVER_H

#include <stddef.h> // per size_t
// #include <stdint.h> // Non più necessario, gestito da event_loop.h

// --- Handle Opaco ---
typedef struct vex_server_ctx_s vex_server_t;

// Forward declaration
struct vex_connection_s;

/**
 * @brief Crea un nuovo contesto server e inizia l'ascolto sulla porta.
 * @param port La porta su cui ascoltare (es. "6379").
 * @return Un puntatore all'handle del server, o NULL in caso di fallimento.
 */
vex_server_t* server_create(const char *port);

/**
 * @brief Avvia il loop eventi principale (Reactor).
 * Questa funzione blocca il thread (non ritorna mai).
 * @param server L'handle del server.
 */
void server_run(vex_server_t *server);

/**
 * @brief Distrugge il server, chiude tutti i socket e libera le risorse.
 * @param server L'handle del server.
 */
void server_destroy(vex_server_t *server);

/**
 * @brief (Interno) Rimuove una connessione dall'array di tracciamento del server.
 * Chiamato da connection_destroy.
 * @param server L'handle del server.
 * @param conn La connessione da rimuovere.
 */
void server_remove_connection(vex_server_t *server, struct vex_connection_s *conn);


/*
 * RIMOSSO: server_register_event.
 * Tutta la registrazione degli eventi è ora gestita
 * dal modulo event_loop e chiamata internamente da server.c
 */
// void server_register_event(vex_server_t *server, int fd, int16_t filter, uint16_t flags, void *udata);


/**
 * @brief (Fase 2) Esegue un comando VSP che è stato parsato con successo.
 * Chiamato dal parser VSP.
 *
 * @param conn La connessione che ha inviato il comando.
 * @param argv Array di stringhe (argomenti del comando).
 * @param argc Numero di argomenti in argv.
 */
void server_execute_command(struct vex_connection_s *conn, char **argv, int argc);


#endif // VEX_SERVER_H