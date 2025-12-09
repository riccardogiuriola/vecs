#
# Vex Project: Makefile (Robust Linker Version)
#

# --- Variabili Base ---
CC = clang
CFLAGS = -Wall -Wextra -std=c11 -g -O2
# Rimuoviamo Werror per sopravvivere ai warning di llama.cpp
# CFLAGS += -Werror 

INCLUDE_DIR = include
SRC_DIR = src
OBJ_DIR = obj
TARGET = vex

# --- Configurazione Llama.cpp ---
LLAMA_ROOT = vendor/llama.cpp
LLAMA_BUILD = $(LLAMA_ROOT)/build

# Include paths
CPPFLAGS = -I$(INCLUDE_DIR) \
           -I$(LLAMA_ROOT)/include \
           -I$(LLAMA_ROOT)/ggml/include

# --- MAGIA DEL LINKER ---
# 1. Trova TUTTI i file .a dentro la cartella build di llama.cpp
#    Questo include libllama.a, libggml.a, libggml-cpu.a, libggml-metal.a, libcommon.a, etc.
ALL_LLAMA_LIBS = $(shell find $(LLAMA_BUILD) -name "*.a")

# --- Gestione Sorgenti ---
ALL_SRCS = $(shell find $(SRC_DIR) -name '*.c')

PLATFORM_SPECIFIC_SRCS = src/net/event_kqueue.c \
                         src/net/event_epoll.c \
                         src/net/event_poll.c

COMMON_SRCS = $(filter-out $(PLATFORM_SPECIFIC_SRCS), $(ALL_SRCS))

# --- Rilevamento OS ---
OS := $(shell uname)

# Librerie comuni (std c++ per llama) + Tutte le lib trovate
LIBS_COMMON = $(ALL_LLAMA_LIBS) -lstdc++

ifeq ($(OS),Darwin)
    PLATFORM_SRC = src/net/event_kqueue.c
    CFLAGS += -D_DARWIN_C_SOURCE
    # Frameworks Apple necessari
    LDFLAGS_PLATFORM = -framework Accelerate -framework Metal -framework Foundation -framework MetalKit
    LIBS = $(LIBS_COMMON) $(LDFLAGS_PLATFORM)

else ifeq ($(OS),Linux)
    PLATFORM_SRC = src/net/event_poll.c
    CFLAGS += -D_GNU_SOURCE
    LDFLAGS_PLATFORM = -lm -pthread
    LIBS = $(LIBS_COMMON) $(LDFLAGS_PLATFORM)
else
    $(error Piattaforma $(OS) non supportata)
endif

ifneq ($(strip $(ALL_LLAMA_LIBS)),)
    # Solo per debug: stampa quali librerie sta linkando
    $(info Linking with external libs: $(ALL_LLAMA_LIBS))
else
    $(error Nessuna libreria statica trovata in $(LLAMA_BUILD). Esegui 'make libs' prima.)
endif

SRCS = $(COMMON_SRCS) $(PLATFORM_SRC)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# --- Target ---
.PHONY: all clean re libs

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "LD   $@"
	@$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	@echo "CC   $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

libs:
	@echo "Compiling Llama.cpp..."
	cd $(LLAMA_ROOT) && cmake -B build -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF && cmake --build build --config Release

clean:
	@echo "CLEAN"
	@rm -rf $(OBJ_DIR) $(TARGET)

re: clean all