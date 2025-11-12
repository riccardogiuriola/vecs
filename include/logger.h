#ifndef vecs_LOGGER_H
#define vecs_LOGGER_H

#include <stdarg.h>

// Livelli di log
typedef enum
{
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

/**
 * @brief Imposta il livello di log minimo da visualizzare.
 */
void logger_set_level(log_level_t level);

/**
 * @brief Logga un messaggio formattato.
 * @param level Livello di severità.
 * @param file Nome del file sorgente (__FILE__).
 ** @param line Numero di riga (__LINE__).
 * @param fmt Stringa di formato (printf-style).
 * @param ... Argomenti per il formato.
 */
void logger_log(log_level_t level, const char *file, int line, const char *fmt, ...);

// Macro helper per un logging più semplice
// Es: log_info("Server avviato sulla porta %d", port);
// Es: log_error("Connessione fallita: %s", strerror(errno));

#define log_debug(...) logger_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...) logger_log(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...) logger_log(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) logger_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) logger_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#endif // vecs_LOGGER_H