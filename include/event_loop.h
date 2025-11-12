/*
 * Vex Project: Header Astrazione Event Loop
 * (include/event_loop.h)
 *
 * Definisce l'API di astrazione per I/O multiplexing (kqueue/epoll).
 *
 * --- AGGIORNATO (Fase 3/Fix) ---
 * Standardizzato su event_loop_t (rimosso vex_event_loop_t)
 */

#ifndef VEX_EVENT_LOOP_H
#define VEX_EVENT_LOOP_H

// Handle opaco per il loop eventi
typedef struct event_loop_s event_loop_t; // <-- CORREZIONE

// Struct evento unificata (ciò che el_poll restituisce)
typedef struct vex_event_s {
    void *udata; // Puntatore utente (es. vex_server_t* o vex_connection_t*)
    int  fd;     // File descriptor associato
    
    // Flags per descrivere l'evento
    unsigned int read : 1;
    unsigned int write : 1;
    unsigned int eof : 1;
    unsigned int error : 1;
} vex_event_t;


/**
 * @brief Crea una nuova istanza del loop eventi (kqueue o epoll).
 * @param max_events Numero massimo di eventi/connessioni da gestire.
 * @return Un puntatore al loop, o NULL in caso di fallimento.
 */
event_loop_t* el_create(int max_events); // <-- CORREZIONE

/**
 * @brief Distrugge il loop eventi e libera le risorse.
 * @param loop Il loop da distruggere.
 */
void el_destroy(event_loop_t *loop); // <-- CORREZIONE

/**
 * @brief Attende che si verifichino eventi di I/O.
 * Blocca il thread finché non ci sono eventi.
 *
 * @param loop Il loop eventi.
 * @param active_events Array (pre-allocato) in cui salvare gli eventi attivi.
 * @param timeout_ms Timeout in millisecondi (-1 per attendere indefinitamente).
 * @return Il numero di eventi attivi in active_events, o -1 in caso di errore.
 */
int el_poll(event_loop_t *loop, vex_event_t *active_events, int timeout_ms); // <-- CORREZIONE

/**
 * @brief Aggiunge un file descriptor al loop per monitorare la LETTURA.
 * @param loop Il loop eventi.
 * @param fd Il file descriptor da aggiungere.
 * @param udata Il puntatore utente da associare (sarà restituito in vex_event_t).
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int el_add_fd_read(event_loop_t *loop, int fd, void *udata); // <-- CORREZIONE

/**
 * @brief Rimuove un file descriptor dal loop.
 * @param loop Il loop eventi.
 * @param fd Il file descriptor da rimuovere.
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int el_del_fd(event_loop_t *loop, int fd); // <-- CORREZIONE

/**
 * @brief Modifica un FD per monitorare la SCRITTURA (oltre alla lettura).
 * @param loop Il loop eventi.
 * @param fd Il file descriptor.
 * @param udata Il puntatore utente (necessario per epoll).
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int el_enable_write(event_loop_t *loop, int fd, void *udata); // <-- CORREZIONE

/**
 * @brief Modifica un FD per smettere di monitorare la SCRITTURA.
 * @param loop Il loop eventi.
 * @param fd Il file descriptor.
 * @param udata Il puntatore utente (necessario per epoll).
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int el_disable_write(event_loop_t *loop, int fd, void *udata); // <-- CORREZIONE


#endif // VEX_EVENT_LOOP_H