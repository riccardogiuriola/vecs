/*
 * Vecs Project: Implementazione System Info
 * (src/utils/sys_info.c)
 */

#include "sys_info.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>

// Necessario per macOS sysctl
#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

// Helper per rimuovere spazi iniziali e finali
static void trim_whitespace(char *str) {
    char *end;
    char *start = str;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != str) memmove(str, start, strlen(start) + 1);
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
}

void sys_get_cpu_model(char *buffer, size_t size) {
    snprintf(buffer, size, "Unknown CPU");

#if defined(__APPLE__)
    size_t len = size;
    if (sysctlbyname("machdep.cpu.brand_string", buffer, &len, NULL, 0) != 0) {
        snprintf(buffer, size, "Apple Silicon / Intel (sysctl failed)");
    }
#elif defined(__linux__)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *start = strchr(line, ':');
                if (start) {
                    start += 2; // Salta ": "
                    strncpy(buffer, start, size - 1);
                    buffer[size - 1] = '\0';
                    trim_whitespace(buffer);
                    break;
                }
            }
        }
        fclose(f);
    }
#endif
}

void sys_get_memory_info(char *buffer, size_t size) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    
    if (pages > 0 && page_size > 0) {
        unsigned long long total_bytes = (unsigned long long)pages * (unsigned long long)page_size;
        double gb = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
        snprintf(buffer, size, "%.2f GB", gb);
    } else {
        snprintf(buffer, size, "Unknown RAM");
    }
}

void sys_get_gpu_info(char *buffer, size_t size) {
    // Default fallback
    snprintf(buffer, size, "Generic GPU Device");

#if defined(__APPLE__)
    // 1. Otteniamo il nome del modello (es. Apple M2 Max)
    char model_name[128] = {0};
    
    // Proviamo system_profiler per il nome commerciale preciso
    FILE *p = popen("system_profiler SPDisplaysDataType 2>/dev/null | grep 'Chipset Model' | head -n 1 | cut -d: -f2", "r");
    if (p) {
        if (fgets(model_name, sizeof(model_name), p)) {
            trim_whitespace(model_name);
        }
        pclose(p);
    }
    
    // Se system_profiler fallisce o è vuoto, fallback su sysctl
    if (strlen(model_name) == 0) {
        size_t len = sizeof(model_name);
        sysctlbyname("machdep.cpu.brand_string", model_name, &len, NULL, 0);
    }

    // 2. Otteniamo la memoria unificata (VRAM effettiva su Apple Silicon)
    int64_t memsize = 0;
    size_t memlen = sizeof(memsize);
    double gb = 0.0;
    if (sysctlbyname("hw.memsize", &memsize, &memlen, NULL, 0) == 0) {
        gb = (double)memsize / (1024.0 * 1024.0 * 1024.0);
    }

    // 3. Formattiamo l'output combinato
    if (gb > 0) {
        snprintf(buffer, size, "%s (%.0f GB Unified)", model_name, gb);
    } else {
        snprintf(buffer, size, "%s (Unified Memory)", model_name);
    }

#elif defined(__linux__)
    // SU LINUX:
    // nvidia-smi restituisce già "Nome, Memoria" (es. Tesla T4, 15360 MiB)
    FILE *p = popen("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null", "r");
    if (p) {
        if (fgets(buffer, size, p)) {
            trim_whitespace(buffer);
        } else {
            snprintf(buffer, size, "GPU Not Detected / Drivers Missing");
        }
        pclose(p);
    }
#endif
}