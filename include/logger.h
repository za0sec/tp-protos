#ifndef LOGGER_H_QwErTyUiOpAsDfGhJkLzXcVbNm
#define LOGGER_H_QwErTyUiOpAsDfGhJkLzXcVbNm

#include <stdio.h>
#include <stdint.h>
#include <netinet/in.h>
#include <time.h>

/**
 * Sistema de Logging para el Servidor SOCKSv5
 *
 * Implementa un registro de acceso que permite a un administrador
 * entender los accesos de cada uno de los usuarios.
 *
 * Formato de log:
 * [TIMESTAMP] [LEVEL] USER@CLIENT_IP -> DEST_ADDR:DEST_PORT STATUS BYTES_TX
 *
 * Ejemplo:
 * [2025-12-04 15:30:45] [ACCESS] admin@192.168.1.100:54321 -> example.com:80 OK 1234
 * [2025-12-04 15:30:46] [ERROR] anonymous@10.0.0.5:12345 -> blocked.com:443 CONN_REFUSED
 */

/** Niveles de log */
typedef enum {
    LOG_LEVEL_DEBUG   = 0,
    LOG_LEVEL_INFO    = 1,
    LOG_LEVEL_ACCESS  = 2,
    LOG_LEVEL_WARNING = 3,
    LOG_LEVEL_ERROR   = 4,
} log_level_t;

/**
 * Inicializa el sistema de logging
 * @param log_file Ruta al archivo de log (NULL = solo stdout)
 * @param min_level Nivel mínimo de log a registrar
 * @return 0 si éxito, -1 si error
 */
int logger_init(const char *log_file, log_level_t min_level);

/**
 * Cierra el sistema de logging
 */
void logger_close(void);

/**
 * Registra un mensaje genérico
 */
void log_message(log_level_t level, const char *format, ...);

/**
 * Registra un acceso de usuario (para el requerimiento 8 del TP)
 * 
 * @param username     Nombre de usuario (o "anonymous")
 * @param client_addr  Dirección del cliente
 * @param dest_addr    Dirección de destino (string)
 * @param dest_port    Puerto de destino
 * @param status       Resultado de la conexión (código SOCKS5)
 * @param bytes_sent   Bytes enviados al destino
 * @param bytes_recv   Bytes recibidos del destino
 */
void log_access(const char *username,
                const struct sockaddr *client_addr,
                const char *dest_addr,
                uint16_t dest_port,
                uint8_t status,
                uint64_t bytes_sent,
                uint64_t bytes_recv);

/**
 * Registra un intento de autenticación
 */
void log_auth(const char *username,
              const struct sockaddr *client_addr,
              int success);

/**
 * Registra una nueva conexión entrante
 */
void log_connection(const struct sockaddr *client_addr, int fd);

/**
 * Registra el cierre de una conexión
 */
void log_disconnection(const struct sockaddr *client_addr, 
                       const char *username,
                       uint64_t duration_ms);

#endif

