/*
 * Vex Project: Implementazione Epoll (Linux)
 * (src/net/event_poll.c)
 */

#ifdef __linux__

#include "event_loop.h"
#include "logger.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>

// Struct interna (definizione dell'handle opaco)
struct event_loop_s {
    int epoll_fd;           // File descriptor di epoll
    int max_events;
    struct epoll_event *events; // Array per gli eventi restituiti da epoll_wait
};

// Funzione helper per epoll_ctl
static int el_epoll_ctl(int epoll_fd, int op, int fd, int events, void *udata) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = udata;
    
    if (epoll_ctl(epoll_fd, op, fd, &ev) == -1) {
        log_error("epoll_ctl(op=%d, fd=%d) fallito: %s", op, fd, strerror(errno));
        return -1;
    }
    return 0;
}

// --- Implementazione API Pubblica ---

event_loop_t* el_create(int max_events) {
    event_loop_t *loop = malloc(sizeof(event_loop_t));
    if (loop == NULL) {
        log_error("malloc fallito per event_loop: %s", strerror(errno));
        return NULL;
    }

    loop->epoll_fd = epoll_create1(0);
    if (loop->epoll_fd == -1) {
        log_error("epoll_create1() fallito: %s", strerror(errno));
        free(loop);
        return NULL;
    }
    
    loop->max_events = max_events;
    loop->events = malloc(sizeof(struct epoll_event) * max_events);
    if (loop->events == NULL) {
        log_error("malloc fallito per array eventi epoll: %s", strerror(errno));
        close(loop->epoll_fd);
        free(loop);
        return NULL;
    }
    
    log_info("Event loop (epoll) creato.");
    return loop;
}

void el_destroy(event_loop_t *loop) {
    if (loop == NULL) return;
    close(loop->epoll_fd);
    free(loop->events);
    free(loop);
}

int el_poll(event_loop_t *loop, vex_event_t *active_events, int timeout_ms) {
    
    // Attende gli eventi
    int num_events = epoll_wait(loop->epoll_fd, loop->events, loop->max_events, timeout_ms);

    if (num_events == -1) {
        if (errno == EINTR) return 0; // Interrotto da segnale
        log_error("epoll_wait() fallito: %s", strerror(errno));
        return -1;
    }

    // Traduce gli eventi epoll nel nostro formato astratto
    for (int i = 0; i < num_events; i++) {
        struct epoll_event *e = &loop->events[i];
        vex_event_t *ve = &active_events[i];
        
        ve->udata = e->data.ptr;
        ve->fd = -1; // Non disponibile direttamente in data.ptr se usiamo puntatori
        
        // Resetta i flag
        ve->read = 0;
        ve->write = 0;
        ve->eof = 0;
        ve->error = 0;

        if (e->events & EPOLLERR) {
            ve->error = 1;
        }
        if (e->events & EPOLLHUP) {
            ve->eof = 1;
        }
        if (e->events & EPOLLRDHUP) { 
            ve->eof = 1;
            ve->read = 1;
        }
        if (e->events & EPOLLIN) {
            ve->read = 1;
        }
        if (e->events & EPOLLOUT) {
            ve->write = 1;
        }
    }
    
    return num_events;
}

int el_add_fd_read(event_loop_t *loop, int fd, void *udata) {
    // Aggiunge FD per LETTURA (EPOLLIN) e Rilevamento Chiusura (EPOLLRDHUP)
    // EPOLLET (Edge-Triggered) per alte prestazioni
    return el_epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, 
                        EPOLLIN | EPOLLRDHUP | EPOLLET, udata);
}

int el_del_fd(event_loop_t *loop, int fd) {
    // Rimuove l'FD da epoll
    return el_epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, 0, NULL);
}

int el_enable_write(event_loop_t *loop, int fd, void *udata) {
    // Modifica (EPOLL_CTL_MOD) per aggiungere EPOLLOUT
    return el_epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, 
                        EPOLLIN | EPOLLRDHUP | EPOLLOUT | EPOLLET, udata);
}

int el_disable_write(event_loop_t *loop, int fd, void *udata) {
    // Modifica (EPOLL_CTL_MOD) per rimuovere EPOLLOUT
    return el_epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, 
                        EPOLLIN | EPOLLRDHUP | EPOLLET, udata);
}

#endif // __linux__