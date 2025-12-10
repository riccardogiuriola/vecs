#ifndef VECS_TEXT_H
#define VECS_TEXT_H

#include <stddef.h> // Necessario per size_t

/**
 * @brief Normalizza il testo per il confronto semantico.
 * Rimuove punteggiatura, spazi extra e converte in minuscolo.
 * * @param input Stringa sorgente (non viene modificata).
 * @param output Buffer di destinazione.
 * @param out_size Dimensione massima del buffer di destinazione.
 */
void normalize_text(const char *input, char *output, size_t out_size);

#endif // VECS_TEXT_H