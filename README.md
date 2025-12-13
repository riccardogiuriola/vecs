# ⚡ VECS (Vector Embedding Caching System)

**VECS** is a high-performance **Semantic Cache** server designed to optimize Large Language Model (LLM) applications.

It acts as a middleware between your user and your LLM (e.g., GPT-4, Claude, Llama 3). Instead of performing expensive and slow inference for every query, VECS stores responses and retrieves them using **Semantic Search**. If a user asks a question similar to one already answered, VECS returns the cached response instantly.

> **Scenario:**
>
> 1.  User A asks: _"How do I reset my password?"_ -> **MISS** -> Call LLM -> Store Answer.
>
> 2.  User B asks: _"I forgot my access key, help"_ -> **HIT** -> Vecs detects similarity (0.85) -> Returns stored answer instantly.

## ✨ Key Features

- **🚀 Native Performance:** Written in pure C with zero runtime dependencies (no Python required).

🧠 Smart Dual-Layer Caching:

- **L1 (Exact Match):** O(1) Hash Map with text normalization (ignores case & punctuation) for instant retrieval.
- **L2 (Hybrid Semantic):** Not just vector search!

  - **Mean Pooling:** Uses state-of-the-art embedding aggregation (not just the [CLS] token) for higher accuracy.
  - **Hybrid Filtering:** Performs keyword analysis to detect negations ("I want..." vs "I do NOT want...") and length mismatch, drastically reducing false positives.

- **⚡ Hardware Acceleration:**

  - **macOS:** Native **Metal (GPU)** support for Apple Silicon (M1/M2/M3).

  - **Linux:** Optimized AVX/AVX2 CPU inference with OpenMP.

- **♻️ Smart Deduplication:** Prevents cache pollution by detecting and rejecting semantically identical entries.

- **🐳 Docker Ready:** Production-ready container with environment configuration.

- **⏱️ TTL Support (New):** Automatic data expiration. Set a Time-To-Live globally via environment variables or individually per specific key.

- **🔌 VSP Protocol:** Simple, text-based TCP protocol (Redis-like).

- **› vecs-cli:** Command line tool for interacting with the VPS protocol.

## 🛠️ Architecture

🤖 **Model Support:**
Vecs automatically detects the model architecture (Encoder-only vs Decoder) to use the correct inference method. It is optimized for modern embedding models like **BGE-M3**, **E5**, and **Nomic-Embed**.

```
graph TD
    Client[Client App] -->|QUERY| Server[Vecs Server]

    subgraph "Vecs Core"
        L1{L1 Exact?}
        AI[Vector Engine (llama.cpp)]
        L2{L2 Semantic?}
        Store[(Vector Store)]
    end

    Server --> L1
    L1 -->|Yes| HitL1[Return Cached Response]
    L1 -->|No| AI

    AI -->|Generate Embedding| L2
    L2 -->|Similarity > Threshold| HitL2[Return Cached Response]
    L2 -->|No Match| Miss[Return MISS]

```

## 📦 Installation (Native)

### Prerequisites

- **Compiler:** GCC or Clang

- **Build Tools:** Make, CMake

- **Git:** For cloning submodules

### 1\. Clone Repository

Clone recursively to include the inference engine (`llama.cpp`).

```
git clone --recursive https://github.com/riccardogiuriola/vecs-client-node
cd vecs

```

### 2\. Download AI Model

Vecs requires a GGUF embedding model. We recommend **BGE-M3** (high accuracy) or **MiniLM-L6-v2** (high speed).

```
mkdir -p models

# Download BGE-M3 (Quantized 4-bit)
curl -L -o models/bge-m3-q4_k_m.gguf https://huggingface.co/gpustack/bge-m3-GGUF/resolve/main/bge-m3-Q4_K_M.gguf

```

### 3\. Build

The `Makefile` automatically detects your OS and links the correct acceleration libraries.

```
# 1. Build dependencies (static libs)
make libs

# 2. Build server executable
make

```

### 4\. Run

```
./vecs

```

_Server listening on port 6379..._

## 🐳 Installation (Docker)

The easiest way to run Vecs in production. The image is approximately **100MB**.

### 1\. Run with Default Settings

This downloads the default model automatically inside the container.

```
docker run -d\
  --name vecs\
  -p 6379:6379\
  vecs:latest

```

### 2\. Run with Custom Configuration

You can tune thresholds and capacity using Environment Variables.

```
docker run -d\
  --name vecs\
  -p 6379:6379\
  -e VECS_L2_THRESHOLD="0.75"\
  -e VECS_L2_DEDUPE_THRESHOLD="0.95"\
  -e VECS_L2_CAPACITY="10000"\
  vecs:latest

```

### 3\. Docker Compose

Create a `docker-compose.yml`:

```
services:
  vecs:
    image: vecs:latest
    build: .
    ports:
      - "6379:6379"
    environment:
      - VECS_L2_THRESHOLD=0.65
      - VECS_L2_CAPACITY=5000
    # Uncomment to use custom models
    # volumes:
    #   - ./my_models:/app/custom_models

```

## ⚙️ Configuration (Environment Variables)

| Variable                   | Default            | Description                                                                              |
| :------------------------- | :----------------- | :--------------------------------------------------------------------------------------- |
| `VECS_MODEL_PATH`          | `models/bge-m3...` | Path to the `.gguf` embedding model.                                                     |
| `VECS_L2_THRESHOLD`        | `0.65`             | Minimum cosine similarity (0.0 - 1.0) to consider a request a HIT. Lower = more lenient. |
| `VECS_L2_DEDUPE_THRESHOLD` | `0.95`             | If a new entry is > 95% similar to an existing one, it is NOT saved (Deduplication).     |
| `VECS_L2_CAPACITY`         | `5000`             | Maximum number of vectors to keep in RAM.                                                |
| `VECS_TTL_DEFAULT`         | `3600`             | Default Time-To-Live in seconds (1 hour) for entries without explicit TTL.               |
| `PORT`.                    | `6379`             | Listening port.                                                                          |

## 📡 API Protocol (VSP)

Vecs uses **VSP (Vecs Simple Protocol)**, a text-based protocol similar to RESP. You can interact with it using `nc` or the provided clients.

### SET (Store Data)

Stores a prompt and its response in L1 (Exact) and L2 (Semantic).

```
SET <Prompt> Metadata_JSON> <Response> [ttl_seconds]
```

### QUERY (Retrieve Data)

Searches L1 first, then calculates embedding and searches L2.

```
QUERY <Prompt> <Metadata_JSON>
```

### DELETE (Remove Data)

Removes exact match from L1 and semantically similar vectors from L2.

```
DELETE <Prompt> <Metadata_JSON>
```

## 💻 Client Libraries

### Node.js / TypeScript

A zero-dependency client is available via npm.

```
npm install vecs-client

```

```
import { VecsClient } from 'vecs-client';

const client = new VecsClient({ host: 'localhost', port: 6379 });
await client.connect();

// Store
await client.set("How do I reset password?", {}, "Go to settings...");

// Query (Semantic)
const answer = await client.query("I forgot my password");
if (answer) console.log("HIT:", answer);

```
