#
# vecs Project: Makefile
#

# Makefile per vecs - C Event-Driven Server

# --- Variabili ---
# Compilatore e flag
CC = clang
# Flag C rigorosi (C11 standard, debug, ottimizzazioni O2, tutti i warning, errori)
CFLAGS = -Wall -Wextra -Werror -std=c11 -g -O2
# Flag C aggiuntivi per lo sviluppo (più pedanti)
# CFLAGS += -Wpedantic -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes

# Directory
INCLUDE_DIR = include
SRC_DIR = src
OBJ_DIR = obj

# Eseguibile
TARGET = vecs

# Trova automaticamente i file sorgente
# AGGIUNTO: src/utils/buffer.c e src/core/connection.c
SRCS = $(wildcard $(SRC_DIR)/core/*.c) \
       $(wildcard $(SRC_DIR)/net/*.c) \
       $(wildcard $(SRC_DIR)/utils/*.c)

# Genera i nomi dei file oggetto (es. obj/core/main.o)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# Flag per il Linker (nessuna libreria esterna per ora)
LDFLAGS =
LIBS =

# Include path per il compilatore
CPPFLAGS = -I$(INCLUDE_DIR)

# --- Target ---

# Target di default: compila l'eseguibile
all: $(TARGET)

# Linka l'eseguibile
$(TARGET): $(OBJS)
	@echo "LD   $@"
	@$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

# Compila i file oggetto
# Crea le directory degli oggetti (es. obj/core) prima di compilare
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	@echo "CC   $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Target di pulizia
clean:
	@echo "CLEAN"
	@rm -rf $(OBJ_DIR) $(TARGET)

# Target per forzare una ricompilazione
re: clean all

# Phony targets (non sono file)
.PHONY: all clean re