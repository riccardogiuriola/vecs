/*
 * vecs Project: Implementazione Socket
 * (src/net/socket.c)
 */

#include "socket.h"
#include "logger.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

int socket_set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        log_error("fcntl(F_GETFL) fallito: %s", strerror(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_error("fcntl(F_SETFL, O_NONBLOCK) fallito: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int socket_create_and_listen(const char *port, int backlog) {
    struct addrinfo hints, *res, *p;
    int listen_fd = -1;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // IPv4 o IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;     // Usa il mio IP

    int status = getaddrinfo(NULL, port, &hints, &res);
    if (status != 0) {
        log_fatal("getaddrinfo fallito: %s", gai_strerror(status));
    }

    for (p = res; p != NULL; p = p->ai_next) {
        listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listen_fd < 0) {
            log_warn("socket() fallito, provo il prossimo...");
            continue;
        }

        // Evita errori "Address already in use"
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            log_fatal("setsockopt(SO_REUSEADDR) fallito: %s", strerror(errno));
        }

        // Imposta non-bloccante PRIMA del bind/listen
        if (socket_set_non_blocking(listen_fd) == -1) {
            close(listen_fd);
            log_fatal("socket_set_non_blocking() fallito");
        }

        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1) {
            close(listen_fd);
            log_warn("bind() fallito: %s, provo il prossimo...", strerror(errno));
            continue;
        }

        // Trovato e bindato con successo
        break;
    }

    freeaddrinfo(res);

    if (p == NULL) {
        log_fatal("Impossibile fare il bind a qualsiasi indirizzo sulla porta %s", port);
    }

    if (listen(listen_fd, backlog) == -1) {
        log_fatal("listen() fallito: %s", strerror(errno));
    }

    return listen_fd;
}