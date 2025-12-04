/**
 * metrics.c - Sistema de métricas del servidor SOCKSv5
 *
 * Implementación de singleton para métricas volátiles
 */
#include "metrics.h"

/** Singleton de métricas (inicializado a cero) */
static struct socks5_metrics metrics = {0};

struct socks5_metrics *
metrics_get(void) {
    return &metrics;
}

void 
metrics_connection_opened(void) {
    metrics.historical_connections++;
    metrics.current_connections++;
}

void 
metrics_connection_closed(void) {
    if(metrics.current_connections > 0) {
        metrics.current_connections--;
    }
}

void 
metrics_add_bytes_from_client(size_t bytes) {
    metrics.bytes_from_clients += bytes;
    metrics.bytes_transferred  += bytes;
}

void 
metrics_add_bytes_to_client(size_t bytes) {
    metrics.bytes_to_clients  += bytes;
    metrics.bytes_transferred += bytes;
}

void 
metrics_add_bytes_from_origin(size_t bytes) {
    metrics.bytes_from_origins += bytes;
    metrics.bytes_transferred  += bytes;
}

void 
metrics_add_bytes_to_origin(size_t bytes) {
    metrics.bytes_to_origins  += bytes;
    metrics.bytes_transferred += bytes;
}

void 
metrics_connection_success(void) {
    metrics.successful_connections++;
}

void 
metrics_connection_failed(void) {
    metrics.failed_connections++;
}

void 
metrics_auth_success(void) {
    metrics.auth_successful++;
}

void 
metrics_auth_failed(void) {
    metrics.auth_failed++;
}

