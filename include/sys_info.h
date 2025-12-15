/*
 * Vecs Project: System Hardware Info Utils
 * (include/sys_info.h)
 */
#ifndef VECS_SYS_INFO_H
#define VECS_SYS_INFO_H

#include <stddef.h>

/**
 * @brief Ottiene il nome del modello della CPU.
 * Scrive nel buffer "Unknown CPU" se non riesce a rilevarlo.
 */
void sys_get_cpu_model(char *buffer, size_t size);

/**
 * @brief Ottiene la RAM totale del sistema formattata (es. "16.00 GB").
 */
void sys_get_memory_info(char *buffer, size_t size);

/**
 * @brief Tenta di ottenere info sulla GPU (via nvidia-smi o detect OS).
 */
void sys_get_gpu_info(char *buffer, size_t size);

#endif // VECS_SYS_INFO_H