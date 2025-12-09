/*
 * Vex Project: Implementazione VSP Parser
 * (src/core/vsp_parser.c)
 */

#include "vsp_parser.h"
#include "buffer.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct vsp_parser_s {
    vsp_parser_state_t state;
    int argc;           // Numero totale di argomenti attesi
    int arg_idx;        // Indice dell'argomento corrente
    int bulk_len;       // Lunghezza dell'argomento (bulk string) corrente
    char **argv;        // Array di argomenti in costruzione
};

vsp_parser_t *vsp_parser_create(void) {
    vsp_parser_t *parser = calloc(1, sizeof(vsp_parser_t));
    if (!parser) {
        log_error("vsp_parser_create: Impossibile allocare memoria per il parser.");
        return NULL;
    }
    // Lo stato (calloc) è già VSP_STATE_INIT
    return parser;
}

void vsp_parser_destroy(vsp_parser_t *parser) {
    if (!parser) return;
    // Assicurati che l'argv parziale sia liberato
    if (parser->argv) {
        vsp_parser_free_argv(parser->arg_idx, parser->argv);
    }
    free(parser);
}

void vsp_parser_free_argv(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

vsp_parser_state_t vsp_parser_get_state(vsp_parser_t *parser) {
    return parser->state;
}

/**
 * @brief Resetta lo stato del parser per il prossimo comando.
 */
static void vsp_parser_reset(vsp_parser_t *parser) {
    // Non liberare argv qui, viene passato al chiamante
    parser->state = VSP_STATE_INIT;
    parser->argc = 0;
    parser->arg_idx = 0;
    parser->bulk_len = 0;
    parser->argv = NULL;
}

/**
 * @brief Tenta di leggere una riga terminata da \r\n dal buffer.
 * Consuma la riga dal buffer se trovata.
 * Restituisce una stringa (da liberare) o NULL se non completa.
 */
static char* vsp_read_line(buffer_t *buf) {
    char *crlf = buffer_find_crlf(buf);
    if (!crlf) {
        return NULL; // Riga non completa
    }

    // Calcola la lunghezza della riga (escluso \r\n)
    size_t line_len = crlf - (char*)buffer_peek(buf);
    
    // Alloca memoria per la riga + terminatore nullo
    char *line = malloc(line_len + 1);
    if (!line) {
        log_error("vsp_read_line: Impossibile allocare memoria per la riga.");
        return NULL;
    }

    // Copia i dati e consuma dal buffer (inclusi \r\n)
    memcpy(line, buffer_peek(buf), line_len);
    line[line_len] = '\0';
    buffer_consume(buf, line_len + 2); // +2 per \r\n
    
    return line;
}


vsp_parse_result_t vsp_parser_execute(vsp_parser_t *parser, buffer_t *buf, int *argc_out, char ***argv_out) {
    char *line = NULL;

    while (1) {
        switch (parser->state) {
            
            case VSP_STATE_INIT:
                // In attesa di '*'
                if (buffer_len(buf) < 1) return VSP_AGAIN;
                
                if (*(char*)buffer_peek(buf) != '*') {
                    parser->state = VSP_STATE_ERROR;
                    log_warn("Errore di protocollo: atteso '*', ricevuto '%c'", *(char*)buffer_peek(buf));
                    return VSP_ERROR;
                }
                buffer_consume(buf, 1);
                parser->state = VSP_STATE_READ_ARGC;
                break; // riesegui il loop

            case VSP_STATE_READ_ARGC:
                // In attesa del numero di argomenti (es. "3\r\n")
                line = vsp_read_line(buf);
                if (!line) return VSP_AGAIN; // Dati incompleti

                parser->argc = atoi(line);
                free(line);
                
                if (parser->argc <= 0) {
                    parser->state = VSP_STATE_ERROR;
                    log_warn("Errore di protocollo: argc non valido (%d)", parser->argc);
                    return VSP_ERROR;
                }

                // Alloca l'array per gli argomenti
                parser->argv = calloc(parser->argc, sizeof(char*));
                if (!parser->argv) {
                    log_error("vsp_parser: Impossibile allocare argv (size: %d)", parser->argc);
                    parser->state = VSP_STATE_ERROR;
                    return VSP_ERROR;
                }
                parser->arg_idx = 0;
                parser->state = VSP_STATE_READ_LEN;
                break; // riesegui il loop

            case VSP_STATE_READ_LEN:
                // In attesa di '$'
                if (buffer_len(buf) < 1) return VSP_AGAIN;
                
                if (*(char*)buffer_peek(buf) != '$') {
                    parser->state = VSP_STATE_ERROR;
                    log_warn("Errore di protocollo: atteso '$', ricevuto '%c'", *(char*)buffer_peek(buf));
                    return VSP_ERROR;
                }
                buffer_consume(buf, 1);
                parser->state = VSP_STATE_READ_BULKLEN;
                break; // riesegui il loop

            case VSP_STATE_READ_BULKLEN:
                // In attesa della lunghezza (es. "5\r\n")
                line = vsp_read_line(buf);
                if (!line) return VSP_AGAIN;

                parser->bulk_len = atoi(line);
                free(line);

                if (parser->bulk_len < 0) {
                    parser->state = VSP_STATE_ERROR;
                    log_warn("Errore di protocollo: lunghezza bulk non valida (%d)", parser->bulk_len);
                    return VSP_ERROR;
                }
                parser->state = VSP_STATE_READ_BULKDATA;
                break; // riesegui il loop

            case VSP_STATE_READ_BULKDATA:
                // In attesa di N byte (dati) + \r\n
                if (buffer_len(buf) < (size_t)parser->bulk_len + 2) {
                    return VSP_AGAIN; // Dati incompleti
                }

                // Alloca e copia i dati
                parser->argv[parser->arg_idx] = malloc(parser->bulk_len + 1);
                if (!parser->argv[parser->arg_idx]) {
                     log_error("vsp_parser: Impossibile allocare argomento (size: %d)", parser->bulk_len);
                    parser->state = VSP_STATE_ERROR;
                    return VSP_ERROR;
                }

                memcpy(parser->argv[parser->arg_idx], buffer_peek(buf), parser->bulk_len);
                parser->argv[parser->arg_idx][parser->bulk_len] = '\0';
                
                // Consuma i dati
                buffer_consume(buf, parser->bulk_len);
                
                // Controlla \r\n finale
                if (*(char*)buffer_peek(buf) != '\r' || *(char*)(buffer_peek(buf) + 1) != '\n') {
                    log_warn("Errore di protocollo: \r\n mancante dopo i dati bulk.");
                    parser->state = VSP_STATE_ERROR;
                    return VSP_ERROR;
                }
                // Consuma \r\n
                buffer_consume(buf, 2);

                // Passa al prossimo argomento
                parser->arg_idx++;
                
                if (parser->arg_idx == parser->argc) {
                    // --- COMANDO COMPLETO ---
                    *argc_out = parser->argc;
                    *argv_out = parser->argv;
                    vsp_parser_reset(parser); // Resetta per il prossimo comando
                    return VSP_OK;
                } else {
                    // Prossimo argomento
                    parser->state = VSP_STATE_READ_LEN;
                    break; // riesegui il loop
                }
            
            case VSP_STATE_ERROR:
                return VSP_ERROR;

            // Questi stati non dovrebbero essere raggiunti nel loop principale
            case VSP_STATE_READ_CR:
            case VSP_STATE_READ_LF:
                parser->state = VSP_STATE_ERROR;
                log_warn("Errore di stato del parser VSP.");
                return VSP_ERROR;
        }
    }
}