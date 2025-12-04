/**
 * main.c - Servidor Proxy SOCKSv5 Concurrente
 *
 * Interpreta los argumentos de línea de comandos y monta los sockets pasivos
 * para el servidor SOCKS5 y el servidor de monitoreo.
 *
 * Todas las conexiones entrantes se manejan en un único hilo usando I/O
 * no bloqueante con un selector (multiplexación).
 *
 * Las operaciones bloqueantes (resolución DNS) se descargan a hilos separados
 * que notifican al selector cuando terminan.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "args.h"
#include "selector.h"
#include "socks5nio.h"
#include "monitoring.h"
#include "metrics.h"
#include "logger.h"

/** Argumentos globales del servidor */
struct socks5args socks5_args;

/** Flag de terminación */
static bool done = false;

static void
sigterm_handler(const int signal) {
    printf("\nSignal %d received, cleaning up and exiting...\n", signal);
    done = true;
}

/**
 * Crea un socket TCP pasivo (escucha) en la dirección y puerto especificados
 */
static int
create_passive_socket(const char *addr, unsigned short port, bool ipv6) {
    int fd = -1;
    int family = ipv6 ? AF_INET6 : AF_INET;
    
    fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if(fd < 0) {
        perror("socket");
        return -1;
    }
    
    // Permitir reusar la dirección
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    
    if(ipv6) {
        struct sockaddr_in6 address;
        memset(&address, 0, sizeof(address));
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        
        if(strcmp(addr, "0.0.0.0") == 0 || strcmp(addr, "::") == 0) {
            address.sin6_addr = in6addr_any;
        } else {
            if(inet_pton(AF_INET6, addr, &address.sin6_addr) != 1) {
                fprintf(stderr, "Invalid IPv6 address: %s\n", addr);
                close(fd);
                return -1;
            }
        }
        
        if(bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("bind (IPv6)");
            close(fd);
            return -1;
        }
    } else {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        
        if(strcmp(addr, "0.0.0.0") == 0) {
            address.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            if(inet_pton(AF_INET, addr, &address.sin_addr) != 1) {
                fprintf(stderr, "Invalid IPv4 address: %s\n", addr);
                close(fd);
                return -1;
            }
        }
        
        if(bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("bind (IPv4)");
            close(fd);
            return -1;
        }
    }
    
    if(listen(fd, 512) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    
    return fd;
}

/**
 * Imprime banner de inicio
 */
static void
print_banner(void) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║           SOCKSv5 Proxy Server - ITBA Protocolos          ║\n");
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  RFC 1928 - SOCKS Protocol Version 5                      ║\n");
    printf("║  RFC 1929 - Username/Password Authentication              ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");
}

int
main(const int argc, char **argv) {
    // Parsear argumentos de línea de comandos
    parse_args(argc, argv, &socks5_args);
    
    print_banner();
    
    // Inicializar sistema de logging
    if(logger_init(socks5_args.log_file, LOG_LEVEL_INFO) != 0) {
        fprintf(stderr, "Warning: could not initialize log file\n");
    }
    if(socks5_args.log_file != NULL) {
        printf("Access log: %s\n", socks5_args.log_file);
    }
    
    // Cerrar stdin (no necesitamos entrada)
    close(STDIN_FILENO);
    
    const char       *err_msg = NULL;
    selector_status   ss      = SELECTOR_SUCCESS;
    fd_selector       selector = NULL;
    
    int server_fd = -1;
    int monitor_fd = -1;
    
    // Registrar manejadores de señales
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);
    signal(SIGPIPE, SIG_IGN);  // Ignorar SIGPIPE
    
    // Crear socket del servidor SOCKS5
    server_fd = create_passive_socket(socks5_args.socks_addr, 
                                       socks5_args.socks_port, false);
    if(server_fd < 0) {
        err_msg = "unable to create SOCKS5 server socket";
        goto finally;
    }
    
    printf("SOCKS5 server listening on %s:%d\n", 
           socks5_args.socks_addr, socks5_args.socks_port);
    
    // Crear socket del servidor de monitoreo
    monitor_fd = create_passive_socket(socks5_args.mng_addr,
                                        socks5_args.mng_port, false);
    if(monitor_fd < 0) {
        err_msg = "unable to create monitoring server socket";
        goto finally;
    }
    
    printf("Monitoring server listening on %s:%d\n",
           socks5_args.mng_addr, socks5_args.mng_port);
    
    // Configurar sockets como no bloqueantes
    if(selector_fd_set_nio(server_fd) == -1) {
        err_msg = "setting server socket non-blocking";
        goto finally;
    }
    
    if(selector_fd_set_nio(monitor_fd) == -1) {
        err_msg = "setting monitoring socket non-blocking";
        goto finally;
    }
    
    // Inicializar el selector
    const struct selector_init conf = {
        .signal = SIGALRM,
        .select_timeout = {
            .tv_sec  = 10,
            .tv_nsec = 0,
        },
    };
    
    if(0 != selector_init(&conf)) {
        err_msg = "initializing selector";
        goto finally;
    }
    
    selector = selector_new(1024);
    if(selector == NULL) {
        err_msg = "unable to create selector";
        goto finally;
    }
    
    // Registrar el servidor SOCKS5
    const struct fd_handler socks5_passive_handler = {
        .handle_read  = socksv5_passive_accept,
        .handle_write = NULL,
        .handle_close = NULL,
    };
    
    ss = selector_register(selector, server_fd, &socks5_passive_handler,
                           OP_READ, NULL);
    if(ss != SELECTOR_SUCCESS) {
        err_msg = "registering SOCKS5 server fd";
        goto finally;
    }
    
    // Registrar el servidor de monitoreo
    const struct fd_handler monitoring_passive_handler = {
        .handle_read  = monitoring_passive_accept,
        .handle_write = NULL,
        .handle_close = NULL,
    };
    
    ss = selector_register(selector, monitor_fd, &monitoring_passive_handler,
                           OP_READ, NULL);
    if(ss != SELECTOR_SUCCESS) {
        err_msg = "registering monitoring server fd";
        goto finally;
    }
    
    // Mostrar usuarios configurados
    printf("\nConfigured users:\n");
    int user_count = 0;
    for(int i = 0; i < MAX_USERS; i++) {
        if(socks5_args.users[i].name != NULL) {
            printf("  - %s\n", socks5_args.users[i].name);
            user_count++;
        }
    }
    if(user_count == 0) {
        printf("  (no authentication required)\n");
    }
    
    printf("\nServer started. Press Ctrl+C to stop.\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");
    
    // Loop principal
    while(!done) {
        err_msg = NULL;
        ss = selector_select(selector);
        if(ss != SELECTOR_SUCCESS) {
            err_msg = "serving";
            goto finally;
        }
    }
    
    if(err_msg == NULL) {
        err_msg = "closing";
    }
    
    int ret = 0;
    
finally:
    if(ss != SELECTOR_SUCCESS) {
        fprintf(stderr, "%s: %s\n", 
                (err_msg == NULL) ? "" : err_msg,
                ss == SELECTOR_IO ? strerror(errno) : selector_error(ss));
        ret = 2;
    } else if(err_msg && strcmp(err_msg, "closing") != 0) {
        perror(err_msg);
        ret = 1;
    }
    
    // Imprimir métricas finales
    struct socks5_metrics *m = metrics_get();
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("Final Statistics:\n");
    printf("  Historical connections: %lu\n", m->historical_connections);
    printf("  Successful connections: %lu\n", m->successful_connections);
    printf("  Failed connections: %lu\n", m->failed_connections);
    printf("  Total bytes transferred: %lu\n", m->bytes_transferred);
    printf("═══════════════════════════════════════════════════════════════\n");
    
    // Limpieza
    if(selector != NULL) {
        selector_destroy(selector);
    }
    selector_close();
    
    socksv5_pool_destroy();
    monitoring_destroy();
    logger_close();
    
    if(server_fd >= 0) {
        close(server_fd);
    }
    if(monitor_fd >= 0) {
        close(monitor_fd);
    }
    
    printf("Server shutdown complete.\n");
    
    return ret;
}
