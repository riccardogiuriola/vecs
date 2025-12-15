/*
 * vecs Project: Implementazione Logger
 * (src/utils/logger.c)
 */

#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>

// Livello di log globale (default: INFO)
static log_level_t global_log_level = LOG_INFO;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

#define COLOR_RESET   "\x1b[0m"
#define COLOR_CYAN    "\x1b[36m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RED     "\x1b[31m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_GREY    "\x1b[90m"

static const char* get_level_color(log_level_t level) {
    switch (level) {
        case LOG_DEBUG: return COLOR_CYAN;
        case LOG_INFO:  return COLOR_GREEN;
        case LOG_WARN:  return COLOR_YELLOW;
        case LOG_ERROR: return COLOR_RED;
        case LOG_FATAL: return COLOR_MAGENTA;
        default: return COLOR_RESET;
    }
}

static const char* get_level_char(log_level_t level) {
    switch (level) {
        case LOG_DEBUG: return "DBG";
        case LOG_INFO:  return "INF";
        case LOG_WARN:  return "WRN";
        case LOG_ERROR: return "ERR";
        case LOG_FATAL: return "FAT";
        default: return "???";
    }
}

void logger_set_level(log_level_t level) {
    global_log_level = level;
}

void logger_log(log_level_t level, const char *file, int line, const char *fmt, ...) {
    if (level < global_log_level) {
        return;
    }

    // Lock per evitare race conditions tra thread
    pthread_mutex_lock(&log_mutex);

    // Orario
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", lt);

    // Output su stderr
    FILE *out_stream = stderr;

    // Colore
    const char *color = get_level_color(level);
    const char *reset = COLOR_RESET;
    const char *lvl_str = get_level_char(level);

    // Formato Pulito: [ORA] [LVL] Messaggio (file:line solo se non INFO)
    fprintf(out_stream, "%s%s [%s]%s ", COLOR_GREY, time_buf, lvl_str, reset);
    
    // Stampa il livello colorato
    fprintf(out_stream, "%s", color);

    va_list args;
    va_start(args, fmt);
    vfprintf(out_stream, fmt, args);
    va_end(args);

    // Dettagli file solo per debug/errori
    if (level != LOG_INFO) {
        fprintf(out_stream, " %s(%s:%d)%s", COLOR_GREY, file, line, reset);
    } else {
        fprintf(out_stream, "%s", reset);
    }

    fprintf(out_stream, "\n");
    fflush(out_stream);

    pthread_mutex_unlock(&log_mutex);

    if (level == LOG_FATAL) {
        exit(EXIT_FAILURE);
    }
}