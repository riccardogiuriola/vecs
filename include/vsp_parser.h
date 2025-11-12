/*
 * Vex Project: Header VSP Parser
 * (include/vsp_parser.h)
 * * API Aggiornata (Fase 3)
 */
#ifndef VEX_VSP_PARSER_H
#define VEX_VSP_PARSER_H

// Forward declarations
typedef struct vsp_parser_s vsp_parser_t;
typedef struct buffer_s buffer_t;

/**
 * @brief Risultati dell'esecuzione del parser
 */
typedef enum {
    VSP_OK,     // Un comando completo è stato analizzato ed è pronto in argc/argv
    VSP_AGAIN,  // Il buffer non contiene ancora un comando completo
    VSP_ERROR   // Errore di protocollo
} vsp_parse_result_t;

/**
 * @brief Stato interno del parser (per i log)
 */
typedef enum {
    VSP_STATE_INIT,         // In attesa di '*' (inizio array)
    VSP_STATE_READ_ARGC,    // In attesa del numero di argomenti
    VSP_STATE_READ_LEN,     // In attesa di '$' (inizio lunghezza)
    VSP_STATE_READ_BULKLEN, // In attesa della lunghezza
    VSP_STATE_READ_BULKDATA,// In attesa dei dati
    VSP_STATE_READ_CR,      // In attesa di \r dopo i dati
    VSP_STATE_READ_LF,      // In attesa di \n dopo i dati
    VSP_STATE_ERROR
} vsp_parser_state_t;


/**
 * @brief Crea una nuova istanza del parser.
 */
vsp_parser_t *vsp_parser_create(void);

/**
 * @brief Distrugge un'istanza del parser.
 */
void vsp_parser_destroy(vsp_parser_t *parser);

/**
 * @brief Esegue il parser sul buffer di lettura.
 * Questa funzione è una state machine che consuma dati dal buffer.
 * Se un comando è completo (VSP_OK), popola argc_out e argv_out.
 * Il chiamante DEVE liberare argv_out usando vsp_parser_free_argv.
 * * @param parser Il parser.
 * @param buf Il buffer di lettura da cui consumare i dati.
 * @param argc_out Puntatore per restituire il numero di argomenti.
 * @param argv_out Puntatore per restituire l'array di stringhe (argomenti).
 * @return VSP_OK, VSP_AGAIN, o VSP_ERROR.
 */
vsp_parse_result_t vsp_parser_execute(vsp_parser_t *parser, buffer_t *buf, int *argc_out, char ***argv_out);

/**
 * @brief Libera la memoria allocata per argv dal parser.
 * * @param argc Il numero di argomenti (da vsp_parser_execute).
 * @param argv L'array di argomenti (da vsp_parser_execute).
 */
void vsp_parser_free_argv(int argc, char **argv);

/**
 * @brief Ottiene lo stato corrente del parser (per debug/errori).
 */
vsp_parser_state_t vsp_parser_get_state(vsp_parser_t *parser);

#endif // VEX_VSP_PARSER_H