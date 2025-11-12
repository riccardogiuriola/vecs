/*
 * Vex Project: Header VSP Parser
 * (include/vsp_parser.h)
 *
 * Definisce l'interfaccia per il parser VSP (Vex Semantic Protocol),
 * ispirato al protocollo RESP di Redis.
 */

#ifndef VEX_VSP_PARSER_H
#define VEX_VSP_PARSER_H

// Forward declaration per evitare include circolare
struct vex_connection_s;

// Handle opaco per lo stato del parser
typedef struct vsp_parser_s vsp_parser_t;

// Risultati dell'esecuzione del parser
typedef enum
{
    VSP_PARSE_OK_COMMAND,     // Un comando completo è stato parsato
    VSP_PARSE_NEED_MORE_DATA, // Dati insufficienti nel buffer, attendere
    VSP_PARSE_ERROR           // Errore di protocollo
} vsp_parse_result_t;

/**
 * @brief Crea un nuovo contesto per il parser VSP.
 * @return Un puntatore al nuovo stato del parser, o NULL in caso di fallimento.
 */
vsp_parser_t *vsp_parser_create(void);

/**
 * @brief Distrugge un contesto del parser e libera le sue risorse.
 * @param parser Il parser da distruggere.
 */
void vsp_parser_destroy(vsp_parser_t *parser);

/**
 * @brief Esegue il parser sul buffer di lettura di una connessione.
 * Tenta di parsare un comando VSP completo. Se ha successo (OK_COMMAND),
 * i comandi vengono estratti e viene chiamato server_execute_command().
 * Il parser consuma i dati dal read_buf man mano che li processa.
 *
 * @param conn La connessione client.
 * @return Uno stato vsp_parse_result_t.
 */
vsp_parse_result_t vsp_parser_execute(struct vex_connection_s *conn);

#endif // VEX_VSP_PARSER_H