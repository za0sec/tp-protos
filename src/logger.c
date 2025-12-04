/**
 * logger.c - Sistema de Logging para el Servidor SOCKSv5
 *
 * Implementa un registro de acceso que permite a un administrador
 * entender los accesos de cada uno de los usuarios.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "logger.h"
#include "netutils.h"

/** Archivo de log */
static FILE *log_file = NULL;
static log_level_t min_level = LOG_LEVEL_INFO;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/** Nombres de niveles */
static const char *level_names[] = {
    "DEBUG",
    "INFO",
    "ACCESS",
    "WARNING",
    "ERROR"
};

int
logger_init(const char *filename, log_level_t level) {
    min_level = level;
    
    if(filename != NULL) {
        log_file = fopen(filename, "a");
        if(log_file == NULL) {
            perror("logger: failed to open log file");
            return -1;
        }
        // Line buffered
        setvbuf(log_file, NULL, _IOLBF, 0);
    }
    
    return 0;
}

void
logger_close(void) {
    pthread_mutex_lock(&log_mutex);
    if(log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}

static void
get_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

static void
write_log(log_level_t level, const char *message) {
    if(level < min_level) {
        return;
    }
    
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));
    
    pthread_mutex_lock(&log_mutex);
    
    // Escribir a stdout
    fprintf(stdout, "[%s] [%s] %s\n", timestamp, level_names[level], message);
    fflush(stdout);
    
    // Escribir a archivo si está abierto
    if(log_file != NULL) {
        fprintf(log_file, "[%s] [%s] %s\n", timestamp, level_names[level], message);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void
log_message(log_level_t level, const char *format, ...) {
    if(level < min_level) {
        return;
    }
    
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    write_log(level, message);
}

void
log_access(const char *username,
           const struct sockaddr *client_addr,
           const char *dest_addr,
           uint16_t dest_port,
           uint8_t status,
           uint64_t bytes_sent,
           uint64_t bytes_recv) {
    
    char client_str[SOCKADDR_TO_HUMAN_MIN];
    sockaddr_to_human(client_str, sizeof(client_str), client_addr);
    
    const char *status_str;
    switch(status) {
        case 0x00: status_str = "OK"; break;
        case 0x01: status_str = "GENERAL_FAILURE"; break;
        case 0x02: status_str = "NOT_ALLOWED"; break;
        case 0x03: status_str = "NET_UNREACHABLE"; break;
        case 0x04: status_str = "HOST_UNREACHABLE"; break;
        case 0x05: status_str = "CONN_REFUSED"; break;
        case 0x06: status_str = "TTL_EXPIRED"; break;
        case 0x07: status_str = "CMD_NOT_SUPPORTED"; break;
        case 0x08: status_str = "ADDR_NOT_SUPPORTED"; break;
        default:   status_str = "UNKNOWN"; break;
    }
    
    char message[512];
    snprintf(message, sizeof(message), 
             "%s@%s -> %s:%u %s TX:%lu RX:%lu",
             username ? username : "anonymous",
             client_str,
             dest_addr,
             dest_port,
             status_str,
             bytes_sent,
             bytes_recv);
    
    write_log(LOG_LEVEL_ACCESS, message);
}

void
log_auth(const char *username,
         const struct sockaddr *client_addr,
         int success) {
    
    char client_str[SOCKADDR_TO_HUMAN_MIN];
    sockaddr_to_human(client_str, sizeof(client_str), client_addr);
    
    char message[256];
    snprintf(message, sizeof(message),
             "AUTH %s from %s: %s",
             username,
             client_str,
             success ? "SUCCESS" : "FAILED");
    
    write_log(success ? LOG_LEVEL_INFO : LOG_LEVEL_WARNING, message);
}

void
log_connection(const struct sockaddr *client_addr, int fd) {
    char client_str[SOCKADDR_TO_HUMAN_MIN];
    sockaddr_to_human(client_str, sizeof(client_str), client_addr);
    
    char message[128];
    snprintf(message, sizeof(message),
             "New connection from %s (fd=%d)",
             client_str, fd);
    
    write_log(LOG_LEVEL_INFO, message);
}

void
log_disconnection(const struct sockaddr *client_addr,
                  const char *username,
                  uint64_t duration_ms) {
    char client_str[SOCKADDR_TO_HUMAN_MIN];
    sockaddr_to_human(client_str, sizeof(client_str), client_addr);
    
    char message[256];
    snprintf(message, sizeof(message),
             "Connection closed: %s@%s duration=%lums",
             username ? username : "anonymous",
             client_str,
             duration_ms);
    
    write_log(LOG_LEVEL_INFO, message);
}

