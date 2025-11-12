/*
 * Vex Project: Implementazione Kqueue (macOS)
 * (src/net/event_kqueue.c)
 *
 * Implementazione dell'API event_loop per macOS/BSD usando kqueue.
 */

// Questo file viene compilato solo su macOS (vedi Makefile)
#if defined(__APPLE__) || defined(__FreeBSD__)

#include "event_loop.h"
#include "logger.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

// Struct interna (definizione dell'handle opaco)
struct event_loop_s { // <-- CORREZIONE (rimosso vex_)
    int kq_fd;             // File descriptor di kqueue
    int max_events;
    struct kevent *events; // Array per gli eventi restituiti da kevent
};

// Funzione helper per modificare kqueue
static int el_kqueue_ctl(int kq_fd, int fd, int16_t filter, uint16_t flags, void *udata) {
    struct kevent change;
    EV_SET(&change, fd, filter, flags, 0, 0, udata);
    
    if (kevent(kq_fd, &change, 1, NULL, 0, NULL) == -1) {
        // CORREZIONE: Se stiamo CANCELLANDO (EV_DELETE) e l'FD
        // non è stato trovato (ENOENT), non è un errore fatale.
        // Succede se proviamo a cancellare EVFILT_WRITE su un FD
        // che era registrato solo per EVFILT_READ.
        if ((flags & EV_DELETE) && errno == ENOENT) {
            log_debug("kevent() ctl: Ignorato ENOENT (benigno) su EV_DELETE (fd: %d, filter: %d)", fd, filter);
            return 0; // Non è un errore
        }

        log_error("kevent() ctl fallito (fd: %d, filter: %d): %s", fd, filter, strerror(errno));
        return -1;
    }
    return 0;
}


// --- Implementazione API Pubblica ---

event_loop_t* el_create(int max_events) { // <-- CORREZIONE
    event_loop_t *loop = malloc(sizeof(event_loop_t)); // <-- CORREZIONE
    if (loop == NULL) {
        log_error("malloc fallito per event_loop: %s", strerror(errno));
        return NULL;
    }

    loop->kq_fd = kqueue();
    if (loop->kq_fd == -1) {
        log_error("kqueue() fallito: %s", strerror(errno));
        free(loop);
        return NULL;
    }
    
    loop->max_events = max_events;
    loop->events = malloc(sizeof(struct kevent) * max_events);
    if (loop->events == NULL) {
        log_error("malloc fallito per array eventi kqueue: %s", strerror(errno));
        close(loop->kq_fd);
        free(loop);
        return NULL;
    }
    
    log_info("Event loop (kqueue) creato.");
    return loop;
}

void el_destroy(event_loop_t *loop) { // <-- CORREZIONE
    if (loop == NULL) return;
    close(loop->kq_fd);
    free(loop->events);
    free(loop);
}

int el_poll(event_loop_t *loop, vex_event_t *active_events, int timeout_ms) { // <-- CORREZIONE
    struct timespec timeout;
    struct timespec *timeout_ptr = NULL;
    
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (timeout_ms % 1000) * 1000000;
        timeout_ptr = &timeout;
    }

    // Attende gli eventi
    int num_events = kevent(loop->kq_fd, NULL, 0, loop->events, loop->max_events, timeout_ptr);

    if (num_events == -1) {
        if (errno == EINTR) return 0; // Interrotto da segnale, non è un errore
        log_error("kevent() wait fallito: %s", strerror(errno));
        return -1;
    }

    // Traduce gli eventi kqueue nel nostro formato astratto
    for (int i = 0; i < num_events; i++) {
        struct kevent *e = &loop->events[i];
        vex_event_t *ve = &active_events[i];
        
        ve->fd = (int)e->ident;
        ve->udata = e->udata;
        
        // Resetta i flag
        ve->read = 0;
        ve->write = 0;
        ve->eof = 0;
        ve->error = 0;

        if (e->flags & EV_ERROR) {
            ve->error = 1;
            // log_warn("Errore kqueue su fd %d: %s", (int)e->ident, strerror(e->data));
        }
        if (e->flags & EV_EOF) {
            ve->eof = 1;
            // EOF implica anche che è leggibile (per leggere 0 byte)
            ve->read = 1; 
        }
        
        if (e->filter == EVFILT_READ) {
            ve->read = 1;
        }
        if (e->filter == EVFILT_WRITE) {
            ve->write = 1;
        }
    }
    
    return num_events;
}

int el_add_fd_read(event_loop_t *loop, int fd, void *udata) { // <-- CORREZIONE
    // Aggiunge l'evento di lettura (EV_ADD) e lo abilita (EV_ENABLE)
    return el_kqueue_ctl(loop->kq_fd, fd, EVFILT_READ, EV_ADD | EV_ENABLE, udata);
}

int el_del_fd(event_loop_t *loop, int fd) { // <-- CORREZIONE
    // kqueue rimuove tutti i filtri per un FD se non specificato
    // Per sicurezza, rimuoviamo esplicitamente lettura e scrittura
    el_kqueue_ctl(loop->kq_fd, fd, EVFILT_READ, EV_DELETE, NULL);
    el_kqueue_ctl(loop->kq_fd, fd, EVFILT_WRITE, EV_DELETE, NULL);
    return 0;
}

int el_enable_write(event_loop_t *loop, int fd, void *udata) { // <-- CORREZIONE
    // Aggiunge/abilita il filtro di scrittura
    return el_kqueue_ctl(loop->kq_fd, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, udata);
}

int el_disable_write(event_loop_t *loop, int fd, void *udata) { // <-- CORREZIONE
    (void)udata; // Non usato da kqueue
    // Rimuove/disabilita il filtro di scrittura
    return el_kqueue_ctl(loop->kq_fd, fd, EVFILT_WRITE, EV_DELETE, NULL);
}


#endif // __APPLE__ || __FreeBSD__