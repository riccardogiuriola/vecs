/* vecs/src/utils/text.c */

#include "text.h"
#include <ctype.h>  // Necessario per tolower, isalnum, isspace
#include <stddef.h> // Necessario per size_t

void normalize_text(const char *input, char *output, size_t out_size) {
    size_t i = 0, j = 0;
    int space_found = 0;

    while (input[i] != '\0' && j < out_size - 1) {
        unsigned char c = (unsigned char)input[i];

        // 1. Lowercase
        c = tolower(c);

        // 2. Mantieni solo alfanumerici e spazi (pulizia rumore)
        if (isalnum(c) || isspace(c)) {
            // 3. Comprimi spazi multipli
            if (isspace(c)) {
                if (!space_found && j > 0) {
                    output[j++] = ' ';
                    space_found = 1;
                }
            } else {
                output[j++] = c;
                space_found = 0;
            }
        }
        i++;
    }

    // Trim finale (toglie spazio in fondo se presente)
    if (j > 0 && output[j-1] == ' ') {
        j--;
    }
    
    output[j] = '\0';
}