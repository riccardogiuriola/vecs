#
# Vecs Project: Makefile (Hybrid Architecture Support)
#

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O3
INCLUDE_DIR = include
SRC_DIR = src
OBJ_DIR = obj
TARGET = vecs

# --- Configurazione Llama.cpp ---
LLAMA_ROOT = vendor/llama.cpp
LLAMA_BUILD = $(LLAMA_ROOT)/build

# Include paths
CPPFLAGS = -I$(INCLUDE_DIR) \
           -I$(LLAMA_ROOT)/include \
           -I$(LLAMA_ROOT)/ggml/include

# --- RILEVAMENTO OS & GPU ---
OS := $(shell uname)
GPU ?= 0

# Configurazione CMake di base per Llama.cpp
CMAKE_ARGS = -DBUILD_SHARED_LIBS=OFF \
             -DLLAMA_BUILD_EXAMPLES=OFF \
             -DLLAMA_BUILD_TESTS=OFF \
             -DLLAMA_BUILD_SERVER=OFF \
             -DLLAMA_CURL=OFF

# --- CONFIGURAZIONE PIATTAFORMA ---

# 1. macOS (Apple Silicon è default)
ifeq ($(OS),Darwin)
    PLATFORM_SRC = src/net/event_kqueue.c
    CFLAGS += -D_DARWIN_C_SOURCE
    
    # Abilita Metal per default su Mac
    CMAKE_ARGS += -DGGML_METAL=ON
    
    # Frameworks Apple necessari
    LDFLAGS_PLATFORM = -framework Accelerate -framework Metal -framework Foundation -framework MetalKit
    
    # Su Mac non usiamo OpenMP di solito, ma pthreads
    LDFLAGS_PLATFORM += -pthread
endif

# 2. Linux
ifeq ($(OS),Linux)
    PLATFORM_SRC = src/net/event_poll.c
    CFLAGS += -D_GNU_SOURCE
    
    # Base Linux flags
    LDFLAGS_PLATFORM = -lm -pthread -ldl

    # --- GPU (CUDA) vs CPU Check ---
    ifeq ($(GPU),1)
        # Compilazione CUDA
        CMAKE_ARGS += -DGGML_CUDA=ON
        
        # Linker flags per CUDA (Assumiamo path standard, aggiusta se necessario)
        # Necessario linkare il runtime CUDA staticamente o dinamicamente
        LDFLAGS_PLATFORM += -L/usr/local/cuda/lib64 -lcudart -lcublas -lculibos
        CFLAGS += -DVecs_USE_GPU
    else
        # Compilazione CPU Standard (OpenMP)
        CMAKE_ARGS += -DGGML_OPENMP=ON
        CFLAGS += -fopenmp
        LDFLAGS_PLATFORM += -fopenmp
    endif
endif

# --- LOGICA LINKER (Trova tutte le .a generate da llama.cpp) ---
NODEPS := clean libs re

ifeq (0, $(words $(findstring $(MAKECMDGOALS), $(NODEPS))))
    RAW_LIBS = $(shell find $(LLAMA_BUILD) -name "*.a" 2>/dev/null)
    LIB_LLAMA = $(filter %libllama.a, $(RAW_LIBS))
    LIB_GGML_ALL = $(filter-out %libllama.a, $(RAW_LIBS))
    ALL_LLAMA_LIBS = $(LIB_LLAMA) $(LIB_GGML_ALL)

    ifeq ($(strip $(ALL_LLAMA_LIBS)),)
        $(error Nessuna libreria trovata in $(LLAMA_BUILD). Esegui 'make libs' prima.)
    endif
endif

# Group Linking: Risolve dipendenze circolari tra le lib statiche di llama/ggml
ifeq ($(OS),Darwin)
    # macOS non supporta --start-group, ma di solito risolve bene le dipendenze statiche
    LINKER_GROUPS = $(ALL_LLAMA_LIBS) -lstdc++
else
    # Linux (GNU ld) ha bisogno dei gruppi per le dipendenze circolari
    LINKER_GROUPS = -Wl,--start-group $(ALL_LLAMA_LIBS) -Wl,--end-group -lstdc++
endif

# Setup finale LIBS
LIBS = $(LINKER_GROUPS) $(LDFLAGS_PLATFORM)

# --- GESTIONE SORGENTI ---
ALL_SRCS = $(shell find $(SRC_DIR) -name '*.c')

# Rimuovi i file specifici delle piattaforme dalla lista comune
EXCLUDE_SRCS = src/net/event_kqueue.c \
               src/net/event_epoll.c \
               src/net/event_poll.c

COMMON_SRCS = $(filter-out $(EXCLUDE_SRCS), $(ALL_SRCS))

SRCS = $(COMMON_SRCS) $(PLATFORM_SRC)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# --- TARGETS ---
.PHONY: all clean re libs dir_guard

all: dir_guard $(TARGET)

dir_guard:
	@mkdir -p $(OBJ_DIR)

$(TARGET): $(OBJS)
	@echo "LD   $@"
	@$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	@echo "CC   $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Target per compilare Llama.cpp
libs:
	@echo "Compiling Llama.cpp with CMake..."
	@echo "Options: $(CMAKE_ARGS)"
	@mkdir -p $(LLAMA_BUILD)
	cd $(LLAMA_ROOT) && cmake -B build $(CMAKE_ARGS) \
		&& cmake --build build --config Release -j$(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

clean:
	@echo "CLEAN Objects"
	@rm -rf $(OBJ_DIR) $(TARGET)

clean-libs:
	@echo "CLEAN Llama.cpp"
	@rm -rf $(LLAMA_BUILD)

re: clean all