#ifndef METRICS_H_XkJiPqRbToWlZnAsYuMdEfGhVcBx
#define METRICS_H_XkJiPqRbToWlZnAsYuMdEfGhVcBx

#include <stdint.h>
#include <stddef.h>

/**
 * Estructura de métricas del servidor SOCKSv5
 * 
 * Accesible globalmente como singleton thread-safe
 */
struct socks5_metrics {
    /** conexiones históricas (total de conexiones aceptadas) */
    uint64_t historical_connections;
    
    /** conexiones concurrentes activas */
    uint64_t current_connections;
    
    /** bytes totales transferidos (entrada + salida) */
    uint64_t bytes_transferred;
    
    /** bytes recibidos de clientes */
    uint64_t bytes_from_clients;
    
    /** bytes enviados a clientes */
    uint64_t bytes_to_clients;
    
    /** bytes recibidos de servidores origen */
    uint64_t bytes_from_origins;
    
    /** bytes enviados a servidores origen */
    uint64_t bytes_to_origins;
    
    /** cantidad de conexiones exitosas (request completados) */
    uint64_t successful_connections;
    
    /** cantidad de conexiones fallidas */
    uint64_t failed_connections;
    
    /** cantidad de autenticaciones exitosas */
    uint64_t auth_successful;
    
    /** cantidad de autenticaciones fallidas */
    uint64_t auth_failed;
};

/**
 * Obtiene el singleton de métricas
 */
struct socks5_metrics *metrics_get(void);

/**
 * Incrementa el contador de conexiones históricas y concurrentes
 */
void metrics_connection_opened(void);

/**
 * Decrementa el contador de conexiones concurrentes
 */
void metrics_connection_closed(void);

/**
 * Registra bytes transferidos desde el cliente
 */
void metrics_add_bytes_from_client(size_t bytes);

/**
 * Registra bytes transferidos hacia el cliente
 */
void metrics_add_bytes_to_client(size_t bytes);

/**
 * Registra bytes transferidos desde el origen
 */
void metrics_add_bytes_from_origin(size_t bytes);

/**
 * Registra bytes transferidos hacia el origen
 */
void metrics_add_bytes_to_origin(size_t bytes);

/**
 * Registra una conexión exitosa
 */
void metrics_connection_success(void);

/**
 * Registra una conexión fallida
 */
void metrics_connection_failed(void);

/**
 * Registra autenticación exitosa
 */
void metrics_auth_success(void);

/**
 * Registra autenticación fallida
 */
void metrics_auth_failed(void);

#endif

