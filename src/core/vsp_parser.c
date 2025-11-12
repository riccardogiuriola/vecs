/*
 * Vex Project: Implementazione VSP Parser
 * (src/core/vsp_parser.c)
 *
 * State machine per il parsing del protocollo VSP (RESP-like).
 */

#include "vsp_parser.h"
#include "connection.h"
#include "server.h"
#include "buffer.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h> // per isdigit

// Stato interno del parser
typedef enum {
    VSP_STATE_INIT,           // In attesa di un nuovo comando (cerca '*')
    VSP_STATE_EXPECT_ARRAY_LEN, // In attesa del numero di argomenti (es. *3\r\n)
    VSP_STATE_EXPECT_BULK_LEN,  // In attesa della lunghezza di un argomento (es. $5\r\n)
    VSP_STATE_EXPECT_BULK_DATA  // In attesa dei dati dell'argomento (es. QUERY\r\n)
} vsp_parser_state_t;

#define VSP_MAX_ARGS 256

// Struct interna (definizione dell'handle opaco)
struct vsp_parser_s {
    vsp_parser_state_t state;
    
    long current_arg_count; // Numero totale di argomenti attesi
    long current_arg_index; // Indice dell'argomento corrente che stiamo parsando
    long current_bulk_len;  // Lunghezza in byte dell'argomento corrente (bulk)
    
    // Buffer temporaneo per gli argomenti parsati
    char **argv;
    size_t *argl; // Lunghezze degli argomenti
};

// --- Funzioni Helper Interne ---

/**
 * @brief Resetta lo stato del parser per prepararlo al prossimo comando.
 * Libera la memoria allocata per argv.
 */
static void vsp_parser_reset(vsp_parser_t *parser) {
    if (parser->argv) {
        for (long i = 0; i < parser->current_arg_index; i++) {
            free(parser->argv[i]);
        }
        free(parser->argv);
        free(parser->argl);
        parser->argv = NULL;
        parser->argl = NULL;
    }
    
    parser->state = VSP_STATE_INIT;
    parser->current_arg_count = 0;
    parser->current_arg_index = 0;
    parser->current_bulk_len = -1;
}

/**
 * @brief Cerca il terminatore di linea \r\n (CRLF) nel buffer.
 * @return Un puntatore al \r (se trovato, \n segue), o NULL se non trovato.
 */
static const char* find_crlf(buffer_t *buf) {
    const char *data = buffer_data(buf);
    size_t len = buffer_len(buf);
    if (len < 2) return NULL;

    for (size_t i = 0; i < len - 1; i++) {
        if (data[i] == '\r' && data[i+1] == '\n') {
            return data + i;
        }
    }
    return NULL;
}

/**
 * @brief Tenta di parsare un numero (long) da una linea terminata da CRLF.
 * Consuma la linea dal buffer se ha successo.
 */
static vsp_parse_result_t parse_long_from_line(buffer_t *buf, char type, long *result) {
    const char *crlf = find_crlf(buf);
    if (crlf == NULL) {
        return VSP_PARSE_NEED_MORE_DATA; // Non abbiamo ancora una linea completa
    }

    const char *start = buffer_data(buf);
    size_t line_len = (crlf - start) + 2; // +2 per \r\n

    if (start[0] != type) {
        log_warn("Errore di protocollo: atteso '%c', ricevuto '%c'", type, start[0]);
        return VSP_PARSE_ERROR;
    }

    // Parsa il numero
    long num = 0;
    int sign = 1;
    size_t i = 1; // Salta il carattere 'type'

    if (i < line_len - 2 && start[i] == '-') {
        sign = -1;
        i++;
    }
    
    if (i == line_len - 2) { // Solo tipo e CRLF (es. "$\r\n")
         log_warn("Errore di protocollo: numero mancante dopo '%c'", type);
         return VSP_PARSE_ERROR;
    }

    while (i < line_len - 2) {
        if (!isdigit(start[i])) {
            log_warn("Errore di protocollo: caratteri non validi in lunghezza");
            return VSP_PARSE_ERROR;
        }
        num = (num * 10) + (start[i] - '0');
        i++;
    }

    *result = num * sign;
    buffer_consume(buf, line_len); // Linea processata, consuma
    return VSP_PARSE_OK_COMMAND; // OK_COMMAND qui significa "linea parsata con successo"
}


// --- Funzioni Pubbliche ---

vsp_parser_t* vsp_parser_create(void) {
    vsp_parser_t *parser = malloc(sizeof(vsp_parser_t));
    if (parser == NULL) {
        log_error("malloc fallito per vsp_parser_t: %s", strerror(errno));
        return NULL;
    }
    
    parser->argv = NULL;
    parser->argl = NULL;
    vsp_parser_reset(parser);
    
    return parser;
}

void vsp_parser_destroy(vsp_parser_t *parser) {
    if (parser == NULL) return;
    vsp_parser_reset(parser); // Libera argv/argl
    free(parser);
}

vsp_parse_result_t vsp_parser_execute(struct vex_connection_s *conn) {
    vsp_parser_t *parser = connection_get_parser(conn);
    buffer_t *read_buf = connection_get_read_buffer(conn);
    vsp_parse_result_t res;

    // Loop finché abbiamo dati e siamo in uno stato processabile
    while (buffer_len(read_buf) > 0) {
        switch (parser->state) {
            
            case VSP_STATE_INIT:
                // All'inizio, ci aspettiamo un array '*'
                parser->state = VSP_STATE_EXPECT_ARRAY_LEN;
                // Non c'è 'break' qui, passiamo direttamente al prossimo stato
            
            case VSP_STATE_EXPECT_ARRAY_LEN:
                res = parse_long_from_line(read_buf, '*', &parser->current_arg_count);
                if (res == VSP_PARSE_NEED_MORE_DATA) return res; // Dati non sufficienti
                if (res == VSP_PARSE_ERROR) {
                    vsp_parser_reset(parser);
                    return res; // Errore di protocollo
                }
                
                if (parser->current_arg_count > VSP_MAX_ARGS || parser->current_arg_count <= 0) {
                     log_warn("Errore protocollo: numero argomenti non valido %ld", parser->current_arg_count);
                     vsp_parser_reset(parser);
                     return VSP_PARSE_ERROR;
                }

                // Alloca spazio per gli argomenti
                parser->argv = calloc(parser->current_arg_count, sizeof(char*));
                parser->argl = calloc(parser->current_arg_count, sizeof(size_t));
                if (parser->argv == NULL || parser->argl == NULL) {
                    log_error("Fallita allocazione argv per il parser: %s", strerror(errno));
                    vsp_parser_reset(parser);
                    return VSP_PARSE_ERROR; // Errore server (out of memory)
                }
                
                parser->current_arg_index = 0;
                parser->state = VSP_STATE_EXPECT_BULK_LEN;
                break; // Passa al prossimo ciclo del while (potremmo non avere altri dati)

            case VSP_STATE_EXPECT_BULK_LEN:
                res = parse_long_from_line(read_buf, '$', &parser->current_bulk_len);
                if (res == VSP_PARSE_NEED_MORE_DATA) return res;
                if (res == VSP_PARSE_ERROR) {
                    vsp_parser_reset(parser);
                    return res;
                }

                if (parser->current_bulk_len < 0) { // Gestisce RESP Nil "$-1\r\n"
                    parser->argv[parser->current_arg_index] = NULL; // Argomento nullo
                    parser->argl[parser->current_arg_index] = 0;
                    parser->current_arg_index++;
                    parser->state = VSP_STATE_EXPECT_BULK_LEN; // Rimane in attesa del prossimo bulk len
                } else {
                    parser->state = VSP_STATE_EXPECT_BULK_DATA;
                }
                break; // Passa al prossimo ciclo

            case VSP_STATE_EXPECT_BULK_DATA:
                // Dobbiamo leggere 'current_bulk_len' byte + 2 byte per '\r\n'
                if (buffer_len(read_buf) < (size_t)parser->current_bulk_len + 2) {
                    return VSP_PARSE_NEED_MORE_DATA; // Non abbiamo ancora tutti i dati + CRLF
                }

                // Alloca e copia i dati
                char *arg_data = malloc(parser->current_bulk_len + 1); // +1 per null terminator
                if (arg_data == NULL) {
                     log_error("Fallita allocazione arg_data: %s", strerror(errno));
                     vsp_parser_reset(parser);
                     return VSP_PARSE_ERROR;
                }
                
                memcpy(arg_data, buffer_data(read_buf), parser->current_bulk_len);
                arg_data[parser->current_bulk_len] = '\0'; // Rende la stringa C-compatibile
                
                // Controlla il CRLF finale
                const char *data = buffer_data(read_buf);
                if (data[parser->current_bulk_len] != '\r' || data[parser->current_bulk_len + 1] != '\n') {
                    log_warn("Errore protocollo: \r\n mancante dopo i dati bulk");
                    free(arg_data);
                    vsp_parser_reset(parser);
                    return VSP_PARSE_ERROR;
                }

                // Consuma i dati + CRLF
                buffer_consume(read_buf, parser->current_bulk_len + 2);

                // Salva l'argomento
                parser->argv[parser->current_arg_index] = arg_data;
                parser->argl[parser->current_arg_index] = parser->current_bulk_len;
                parser->current_arg_index++;

                // Abbiamo finito di parsare tutti gli argomenti?
                if (parser->current_arg_index == parser->current_arg_count) {
                    // SÌ! Comando completo.
                    // Esegui il comando
                    server_execute_command(conn, parser->argv, parser->current_arg_count);
                    
                    // Resetta per il prossimo comando
                    vsp_parser_reset(parser);
                    // Continua il loop del 'while' se ci sono altri dati nel buffer
                } else {
                    // NO. Torna ad aspettare il prossimo bulk len
                    parser->state = VSP_STATE_EXPECT_BULK_LEN;
                }
                break;
        } // fine switch(state)
    } // fine while(buffer_len > 0)

    return VSP_PARSE_NEED_MORE_DATA; // Finiti i dati nel buffer
}