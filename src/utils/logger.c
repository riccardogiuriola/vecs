/*
 * vex Project: Implementazione Logger
 * (src/utils/logger.c)
 */

#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Livello di log globale (default: INFO)
static log_level_t global_log_level = LOG_INFO;

static const char* log_level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

void logger_set_level(log_level_t level) {
    global_log_level = level;
}

void logger_log(log_level_t level, const char *file, int line, const char *fmt, ...) {
    if (level < global_log_level) {
        return;
    }

    // Ottieni l'orario corrente
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char time_buf[20];
    time_buf[strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", lt)] = '\0';

    // Output su stderr per i log
    FILE *out_stream = stderr;

    // Formatta il messaggio
    fprintf(out_stream, "%s [%-5s] (%s:%d) - ", 
            time_buf, log_level_strings[level], file, line);

    va_list args;
    va_start(args, fmt);
    vfprintf(out_stream, fmt, args);
    va_end(args);

    fprintf(out_stream, "\n");
    fflush(out_stream);

    // Termina l'applicazione in caso di errore fatale
    if (level == LOG_FATAL) {
        exit(EXIT_FAILURE);
    }
}