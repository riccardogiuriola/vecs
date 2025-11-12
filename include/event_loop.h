/*
 * Vex Project: Header Astrazione Event Loop
 * (include/event_loop.h)
 *
 * Definisce l'API di astrazione per I/O multiplexing (kqueue/epoll).
 */

#ifndef VEX_EVENT_LOOP_H
#define VEX_EVENT_LOOP_H

// Handle opaco per il loop eventi
typedef struct vex_event_loop_s vex_event_loop_t;

// Struct evento unificata (ciò che el_poll restituisce)
typedef struct vex_event_s
{
    void *udata; // Puntatore utente (es. vex_server_t* o vex_connection_t*)
    int fd;      // File descriptor associato

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
vex_event_loop_t *el_create(int max_events);

/**
 * @brief Distrugge il loop eventi e libera le risorse.
 * @param loop Il loop da distruggere.
 */
void el_destroy(vex_event_loop_t *loop);

/**
 * @brief Attende che si verifichino eventi di I/O.
 * Blocca il thread finché non ci sono eventi.
 *
 * @param loop Il loop eventi.
 * @param active_events Array (pre-allocato) in cui salvare gli eventi attivi.
 * @param timeout_ms Timeout in millisecondi (-1 per attendere indefinitamente).
 * @return Il numero di eventi attivi in active_events, o -1 in caso di errore.
 */
int el_poll(vex_event_loop_t *loop, vex_event_t *active_events, int timeout_ms);

/**
 * @brief Aggiunge un file descriptor al loop per monitorare la LETTURA.
 * @param loop Il loop eventi.
 * @param fd Il file descriptor da aggiungere.
 * @param udata Il puntatore utente da associare (sarà restituito in vex_event_t).
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int el_add_fd_read(vex_event_loop_t *loop, int fd, void *udata);

/**
 * @brief Rimuove un file descriptor dal loop.
 * @param loop Il loop eventi.
 * @param fd Il file descriptor da rimuovere.
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int el_del_fd(vex_event_loop_t *loop, int fd);

/**
 * @brief Modifica un FD per monitorare la SCRITTURA (oltre alla lettura).
 * @param loop Il loop eventi.
 * @param fd Il file descriptor.
 * @param udata Il puntatore utente (necessario per epoll).
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int el_enable_write(vex_event_loop_t *loop, int fd, void *udata);

/**
 * @brief Modifica un FD per smettere di monitorare la SCRITTURA.
 * @param loop Il loop eventi.
 * @param fd Il file descriptor.
 * @param udata Il puntatore utente (necessario per epoll).
 * @return 0 in caso di successo, -1 in caso di errore.
 */
int el_disable_write(vex_event_loop_t *loop, int fd, void *udata);

#endif // VEX_EVENT_LOOP_H