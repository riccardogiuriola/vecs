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

# --- Rilevamento OS per Portabilità ---
# Controlla l'Operating System (Darwin = macOS)
OS := $(shell uname)

# File sorgente comuni a tutte le piattaforme
COMMON_SRCS = $(wildcard $(SRC_DIR)/core/*.c) \
              $(wildcard $(SRC_DIR)/net/socket.c) \
              $(wildcard $(SRC_DIR)/utils/*.c)

# File sorgente specifici della piattaforma
ifeq ($(OS),Darwin)
    PLATFORM_SRCS = src/net/event_kqueue.c
    CFLAGS += -D_DARWIN_C_SOURCE # Definisce per macOS
else ifeq ($(OS),Linux)
    PLATFORM_SRCS = src/net/event_epoll.c
    CFLAGS += -D_GNU_SOURCE # Definisce per Linux (per epoll)
else
    $(error Piattaforma $(OS) non supportata)
endif

# Lista sorgenti completa
SRCS = $(COMMON_SRCS) $(PLATFORM_SRCS)

# Genera i nomi dei file oggetto (es. obj/core/main.o)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))


# --- Target ---
.PHONY: all clean re

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "LD   $@"
	@$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	@echo "CC   $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	@echo "CLEAN"
	@rm -rf $(OBJ_DIR) $(TARGET)

re: clean all