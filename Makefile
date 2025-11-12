#
# Vex Project: Makefile
#

# --- Variabili ---
CC = clang
CFLAGS = -Wall -Wextra -Werror -std=c11 -g -O2
INCLUDE_DIR = include
SRC_DIR = src
OBJ_DIR = obj
TARGET = vex
LDFLAGS =
LIBS =
CPPFLAGS = -I$(INCLUDE_DIR)

# --- Gestione Sorgenti (Robust) ---
# 1. Trova TUTTI i file .c
ALL_SRCS = $(shell find $(SRC_DIR) -name '*.c')

# 2. Definisci i file che dipendono dalla piattaforma
PLATFORM_SPECIFIC_SRCS = src/net/event_kqueue.c \
                         src/net/event_epoll.c \
                         src/net/event_poll.c # <-- MODIFICA (Aggiunto event_poll.c)

# 3. I sorgenti comuni sono TUTTI meno quelli specifici
COMMON_SRCS = $(filter-out $(PLATFORM_SPECIFIC_SRCS), $(ALL_SRCS))

# --- Rilevamento OS per Portabilità ---
OS := $(shell uname)

# 4. Seleziona il file corretto per QUESTA piattaforma
ifeq ($(OS),Darwin)
    PLATFORM_SRC = src/net/event_kqueue.c
    CFLAGS += -D_DARWIN_C_SOURCE # Definisce per macOS
else ifeq ($(OS),Linux)
    PLATFORM_SRC = src/net/event_poll.c # <-- MODIFICA (Cambiato da epoll a poll)
    CFLAGS += -D_GNU_SOURCE # Definisce per Linux (per poll)
else
    $(error Piattaforma $(OS) non supportata)
endif

# 5. Lista sorgenti completa
SRCS = $(COMMON_SRCS) $(PLATFORM_SRC)

# Genera i nomi dei file oggetto (es. obj/core/main.o)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))


# --- Target ---
.PHONY: all clean re

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "LD   $@"
	@$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

# Regola di pattern per compilare i .o nelle loro sottodirectory obj/
# (es. src/core/main.c -> obj/core/main.o)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	@echo "CC   $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	@echo "CLEAN"
	@rm -rf $(OBJ_DIR) $(TARGET)

re: clean all