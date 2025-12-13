# ==========================================
# Stage 1: Builder
# ==========================================
FROM debian:bookworm-slim AS builder

# Installiamo anche libcurl4-openssl-dev per sicurezza
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    curl \
    python3 \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# 1. CLEANUP: Rimuove artefatti locali macOS per evitare conflitti
RUN rm -rf vendor/llama.cpp/build

# 2. Download Modello (BGE-M3)
#RUN mkdir -p models && \
#    curl -L -o models/default_model.gguf \
#    https://huggingface.co/gpustack/bge-m3-GGUF/resolve/main/bge-m3-Q4_K_M.gguf

# 3. Build Llama.cpp (Usa il nuovo Makefile con CURL=OFF)
RUN make libs

# 4. Build Server
RUN make -j$(nproc)

# 4. Build CLI
# Usa -C per dire a make di spostarsi nella directory
RUN make -C vecs-cli -j$(nproc)

# ==========================================
# Stage 2: Runtime (Minimal)
# ==========================================
FROM debian:bookworm-slim

WORKDIR /app

# Installiamo libgomp1 (OpenMP) necessario per l'inferenza CPU su Linux
RUN apt-get update && apt-get install -y \
    libgomp1 \
    && rm -rf /var/lib/apt/lists/*

# Copia il server compilato
COPY --from=builder /app/vecs .
# Copia CLI compilata in un percorso globale
COPY --from=builder /app/vecs-cli/vecs-cli /usr/local/bin/vecs-cli

RUN mkdir -p /app/data && \
    mkdir -p /app/models

# ENV Defaults
ENV VECS_MODEL_PATH="/app/models/default_model.gguf"
ENV VECS_L2_THRESHOLD="0.65"
ENV VECS_L2_DEDUPE_THRESHOLD="0.95"
ENV VECS_L2_CAPACITY="5000"

EXPOSE 6379
VOLUME ["/app/data", "/app/models"]

CMD ["./vecs"]