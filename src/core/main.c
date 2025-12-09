/*
 * vex Project: Entry Point Main
 * (src/core/main.c)
 */

#include "server.h"
#include "logger.h"
#include <stdlib.h>

#define DEFAULT_PORT "6379"

int main(void) {
    // Imposta il livello di log
    logger_set_level(LOG_DEBUG);

    log_info("Avvio di vex Semantic Cache Proxy...");

    // 1. Crea il server (alloca contesto, apre kqueue, bind+listen socket)
    vex_server_t *server = server_create(DEFAULT_PORT);

    if (server == NULL) {
        // log_fatal è già stato chiamato da server_create o dalle sue dipendenze
        return EXIT_FAILURE;
    }

    // 2. Avvia il loop eventi bloccante (Reactor)
    // Questa funzione non ritornerà finché il server è in esecuzione.
    server_run(server);

    // 3. Cleanup (raggiunto solo se server_run esce,
    //    ad esempio gestendo SIGINT, cosa non implementata in questo MVP)
    log_info("Cleanup del server...");
    server_destroy(server);

    return EXIT_SUCCESS;
}