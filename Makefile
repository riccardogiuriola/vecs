#
# Vecs Project: Makefile (Docker & Linker Fix + OpenMP)
#

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O2
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

# --- LOGICA LINKER ---
NODEPS := clean libs

ifeq (0, $(words $(findstring $(MAKECMDGOALS), $(NODEPS))))
    # Recursive search for .a files
    RAW_LIBS = $(shell find $(LLAMA_BUILD) -name "*.a" 2>/dev/null)
    
    LIB_LLAMA = $(filter %libllama.a, $(RAW_LIBS))
    LIB_GGML_ALL = $(filter-out %libllama.a, $(RAW_LIBS))
    
    ALL_LLAMA_LIBS = $(LIB_LLAMA) $(LIB_GGML_ALL)

    ifneq ($(strip $(ALL_LLAMA_LIBS)),)
        # OK
    else
        $(error Nessuna libreria trovata in $(LLAMA_BUILD). Esegui 'make libs' prima.)
    endif
endif

# --- Gestione Sorgenti ---
ALL_SRCS = $(shell find $(SRC_DIR) -name '*.c')

PLATFORM_SPECIFIC_SRCS = src/net/event_kqueue.c \
                         src/net/event_epoll.c \
                         src/net/event_poll.c

COMMON_SRCS = $(filter-out $(PLATFORM_SPECIFIC_SRCS), $(ALL_SRCS))

OS := $(shell uname)

# Linker Flags Comuni
LINKER_GROUPS = -Wl,--start-group $(ALL_LLAMA_LIBS) -Wl,--end-group -lstdc++

ifeq ($(OS),Darwin)
    PLATFORM_SRC = src/net/event_kqueue.c
    CFLAGS += -D_DARWIN_C_SOURCE
    LDFLAGS_PLATFORM = -framework Accelerate -framework Metal -framework Foundation -framework MetalKit
    LIBS = $(ALL_LLAMA_LIBS) -lstdc++ $(LDFLAGS_PLATFORM)
else
    # Linux / Docker
    PLATFORM_SRC = src/net/event_poll.c
    CFLAGS += -D_GNU_SOURCE
    LDFLAGS_PLATFORM = -lm -pthread -ldl -fopenmp
    
    LIBS = $(LINKER_GROUPS) $(LDFLAGS_PLATFORM)
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

# Target libs
libs:
	@echo "Compiling Llama.cpp..."
	@mkdir -p $(LLAMA_BUILD)
	cd $(LLAMA_ROOT) && cmake -B build \
		-DBUILD_SHARED_LIBS=OFF \
		-DLLAMA_BUILD_EXAMPLES=OFF \
		-DLLAMA_BUILD_TESTS=OFF \
		-DLLAMA_BUILD_SERVER=OFF \
		-DLLAMA_CURL=OFF \
		-DGGML_NATIVE=OFF \
		&& cmake --build build --config Release --target llama -j

clean:
	@echo "CLEAN"
	@rm -rf $(OBJ_DIR) $(TARGET)

re: clean all