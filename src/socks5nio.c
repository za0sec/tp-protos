/**
 * socks5nio.c - Proxy SOCKSv5 con I/O no bloqueante
 *
 * Implementa la máquina de estados completa del protocolo SOCKSv5 (RFC 1928)
 * con soporte para autenticación usuario/contraseña (RFC 1929).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

#include "hello.h"
#include "auth.h"
#include "request.h"
#include "buffer.h"
#include "stm.h"
#include "socks5nio.h"
#include "netutils.h"
#include "args.h"
#include "metrics.h"
#include "logger.h"

#define N(x) (sizeof(x)/sizeof((x)[0]))

/** Tamaño de los buffers de I/O */
#define BUFFER_SIZE 4096

/** Versión de SOCKS */
#define SOCKS_VERSION 0x05

/** Argumentos globales del servidor */
extern struct socks5args socks5_args;

////////////////////////////////////////////////////////////////////////////////
// DEFINICIÓN DE ESTADOS
////////////////////////////////////////////////////////////////////////////////

/** Máquina de estados general */
enum socks_v5state {
    /**
     * Recibe el mensaje `hello` del cliente y lo procesa
     * Intereses: OP_READ sobre client_fd
     * Transiciones:
     *   - HELLO_READ mientras el mensaje no esté completo
     *   - HELLO_WRITE cuando está completo
     *   - ERROR ante cualquier error
     */
    HELLO_READ,

    /**
     * Envía la respuesta del `hello` al cliente
     * Intereses: OP_WRITE sobre client_fd
     * Transiciones:
     *   - HELLO_WRITE mientras queden bytes por enviar
     *   - AUTH_READ si se negoció autenticación
     *   - REQUEST_READ si no requiere autenticación
     *   - ERROR ante cualquier error
     */
    HELLO_WRITE,

    /**
     * Lee credenciales de autenticación (RFC 1929)
     * Intereses: OP_READ sobre client_fd
     * Transiciones:
     *   - AUTH_READ mientras no esté completo
     *   - AUTH_WRITE cuando esté completo
     *   - ERROR ante cualquier error
     */
    AUTH_READ,

    /**
     * Envía respuesta de autenticación
     * Intereses: OP_WRITE sobre client_fd
     * Transiciones:
     *   - AUTH_WRITE mientras queden bytes
     *   - REQUEST_READ si autenticación exitosa
     *   - ERROR si falló
     */
    AUTH_WRITE,

    /**
     * Lee el request del cliente (CONNECT)
     * Intereses: OP_READ sobre client_fd
     * Transiciones:
     *   - REQUEST_READ mientras no esté completo
     *   - REQUEST_RESOLVING si es FQDN
     *   - REQUEST_CONNECTING si es IP directa
     *   - ERROR ante cualquier error
     */
    REQUEST_READ,

    /**
     * Resuelve DNS de forma asíncrona
     * Intereses: OP_NOOP (espera señal del hilo DNS)
     * Transiciones:
     *   - REQUEST_CONNECTING cuando termine la resolución
     *   - REQUEST_WRITE si falla la resolución
     */
    REQUEST_RESOLVING,

    /**
     * Conecta al servidor origen
     * Intereses: OP_WRITE sobre origin_fd
     * Transiciones:
     *   - REQUEST_CONNECTING mientras no conecte
     *   - REQUEST_WRITE cuando conecte o falle
     */
    REQUEST_CONNECTING,

    /**
     * Envía respuesta del request al cliente
     * Intereses: OP_WRITE sobre client_fd
     * Transiciones:
     *   - REQUEST_WRITE mientras queden bytes
     *   - COPY si fue exitoso
     *   - DONE/ERROR si falló
     */
    REQUEST_WRITE,

    /**
     * Copia bytes bidireccionalmente (túnel)
     * Intereses: OP_READ/OP_WRITE según disponibilidad
     * Transiciones:
     *   - COPY mientras haya datos
     *   - DONE cuando se cierre un extremo
     */
    COPY,

    // Estados terminales
    DONE,
    ERROR,
};

////////////////////////////////////////////////////////////////////////////////
// ESTRUCTURAS PARA CADA ESTADO
////////////////////////////////////////////////////////////////////////////////

/** Usado por HELLO_READ, HELLO_WRITE */
struct hello_st {
    buffer              *rb, *wb;
    struct hello_parser  parser;
    uint8_t              method;
};

/** Usado por AUTH_READ, AUTH_WRITE */
struct auth_st {
    buffer             *rb, *wb;
    struct auth_parser  parser;
    uint8_t             status;  // 0x00 = success
};

/** Usado por REQUEST_READ, REQUEST_WRITE */
struct request_st {
    buffer                *rb, *wb;
    struct request_parser  parser;
    struct request         request;
    
    /** resolución DNS */
    struct addrinfo       *origin_resolution;
    struct addrinfo       *current_origin;
    
    /** estado de la conexión */
    enum socks_reply_status status;
};

/** Usado por REQUEST_CONNECTING */
struct connecting {
    buffer   *wb;
    int       fd;
    enum socks_reply_status status;
};

/** Usado por COPY */
struct copy {
    int       *fd;
    buffer    *rb, *wb;
    fd_interest duplex;
    struct copy *other;
};

////////////////////////////////////////////////////////////////////////////////
// ESTRUCTURA PRINCIPAL DE CONEXIÓN
////////////////////////////////////////////////////////////////////////////////

struct socks5 {
    /** información del cliente */
    struct sockaddr_storage  client_addr;
    socklen_t                client_addr_len;
    int                      client_fd;

    /** información del servidor origen */
    struct sockaddr_storage  origin_addr;
    socklen_t                origin_addr_len;
    int                      origin_fd;
    struct addrinfo         *origin_resolution;
    struct addrinfo         *origin_resolution_current;

    /** máquinas de estados */
    struct state_machine     stm;

    /** estados para el client_fd */
    union {
        struct hello_st      hello;
        struct auth_st       auth;
        struct request_st    request;
        struct copy          copy;
    } client;

    /** estados para el origin_fd */
    union {
        struct connecting    conn;
        struct copy          copy;
    } orig;

    /** buffers para I/O */
    uint8_t raw_buff_a[BUFFER_SIZE], raw_buff_b[BUFFER_SIZE];
    buffer  read_buffer, write_buffer;

    /** contador de referencias */
    unsigned references;

    /** siguiente en el pool */
    struct socks5 *next;

    /** username autenticado (para logging) */
    char username[257];
    
    /** Para logging de acceso */
    time_t      connection_start;
    char        dest_addr_str[256];
    uint16_t    dest_port;
    uint8_t     last_status;
    uint64_t    bytes_to_origin;
    uint64_t    bytes_from_origin;
};

////////////////////////////////////////////////////////////////////////////////
// POOL DE CONEXIONES
////////////////////////////////////////////////////////////////////////////////

static const unsigned max_pool = 50;
static unsigned pool_size = 0;
static struct socks5 *pool = NULL;

/** Forward declaration de la tabla de estados (ERROR + 1 = 11 estados) */
static const struct state_definition socks5_state_handlers[ERROR + 1];

static struct socks5 *
socks5_new(int client_fd) {
    struct socks5 *ret;

    if(pool == NULL) {
        ret = malloc(sizeof(*ret));
    } else {
        ret       = pool;
        pool      = pool->next;
        ret->next = NULL;
        pool_size--;
    }

    if(ret == NULL) {
        goto finally;
    }

    memset(ret, 0x00, sizeof(*ret));

    ret->client_fd = client_fd;
    ret->origin_fd = -1;

    ret->stm.initial   = HELLO_READ;
    ret->stm.max_state = ERROR;
    ret->stm.states    = socks5_state_handlers;
    stm_init(&ret->stm);

    buffer_init(&ret->read_buffer,  N(ret->raw_buff_a), ret->raw_buff_a);
    buffer_init(&ret->write_buffer, N(ret->raw_buff_b), ret->raw_buff_b);

    ret->references = 1;
    ret->connection_start = time(NULL);
    ret->dest_addr_str[0] = '\0';
    ret->dest_port = 0;
    ret->last_status = 0xFF;
    ret->bytes_to_origin = 0;
    ret->bytes_from_origin = 0;

finally:
    return ret;
}

static void
socks5_destroy_(struct socks5* s) {
    if(s->origin_resolution != NULL) {
        freeaddrinfo(s->origin_resolution);
        s->origin_resolution = NULL;
    }
    free(s);
}

static void
socks5_destroy(struct socks5 *s) {
    if(s == NULL) {
        // nada para hacer
    } else if(s->references == 1) {
        if(pool_size < max_pool) {
            s->next = pool;
            pool    = s;
            pool_size++;
        } else {
            socks5_destroy_(s);
        }
    } else {
        s->references -= 1;
    }
}

void
socksv5_pool_destroy(void) {
    struct socks5 *next, *s;
    for(s = pool; s != NULL ; s = next) {
        next = s->next;
        free(s);
    }
    pool = NULL;
    pool_size = 0;
}

/** obtiene el struct (socks5 *) desde la llave de selección */
#define ATTACHMENT(key) ((struct socks5 *)(key)->data)

////////////////////////////////////////////////////////////////////////////////
// DECLARACIONES FORWARD
////////////////////////////////////////////////////////////////////////////////

static void socksv5_read   (struct selector_key *key);
static void socksv5_write  (struct selector_key *key);
static void socksv5_block  (struct selector_key *key);
static void socksv5_close  (struct selector_key *key);
static void socksv5_done   (struct selector_key *key);

static const struct fd_handler socks5_handler = {
    .handle_read   = socksv5_read,
    .handle_write  = socksv5_write,
    .handle_close  = socksv5_close,
    .handle_block  = socksv5_block,
};

////////////////////////////////////////////////////////////////////////////////
// HELLO
////////////////////////////////////////////////////////////////////////////////

/** Callback del parser: invocado por cada método de autenticación */
static void
on_hello_method(struct hello_parser *p, const uint8_t method) {
    uint8_t *selected = p->data;

    // Verificar si se requiere autenticación
    bool auth_required = false;
    for(int i = 0; i < MAX_USERS; i++) {
        if(socks5_args.users[i].name != NULL) {
            auth_required = true;
            break;
        }
    }

    if(auth_required) {
        // Si hay usuarios configurados, requerir USER/PASS
        if(method == SOCKS_HELLO_USERNAME_PASSWORD) {
            *selected = method;
        }
    } else {
        // Si no hay usuarios, aceptar NO AUTH
        if(method == SOCKS_HELLO_NOAUTHENTICATION_REQUIRED) {
            *selected = method;
        }
    }
}

/** Inicializa las variables de los estados HELLO */
static void
hello_read_init(const unsigned state, struct selector_key *key) {
    (void) state;
    struct hello_st *d = &ATTACHMENT(key)->client.hello;

    d->rb                              = &ATTACHMENT(key)->read_buffer;
    d->wb                              = &ATTACHMENT(key)->write_buffer;
    d->method                          = SOCKS_HELLO_NO_ACCEPTABLE_METHODS;
    d->parser.data                     = &d->method;
    d->parser.on_authentication_method = on_hello_method;
    hello_parser_init(&d->parser);
}

static void
hello_read_close(const unsigned state, struct selector_key *key) {
    (void) state;
    struct hello_st *d = &ATTACHMENT(key)->client.hello;
    hello_parser_close(&d->parser);
}

/** Procesa el mensaje hello y arma la respuesta */
static unsigned
hello_process(struct hello_st *d) {
    unsigned ret = HELLO_WRITE;

    uint8_t m = d->method;
    if(-1 == hello_marshall(d->wb, m)) {
        ret = ERROR;
    }
    if(m == SOCKS_HELLO_NO_ACCEPTABLE_METHODS) {
        ret = ERROR;
    }

    return ret;
}

/** Lee bytes del mensaje hello */
static unsigned
hello_read(struct selector_key *key) {
    struct hello_st *d = &ATTACHMENT(key)->client.hello;
    unsigned  ret      = HELLO_READ;
    bool      error    = false;
    uint8_t  *ptr;
    size_t    count;
    ssize_t   n;

    ptr = buffer_write_ptr(d->rb, &count);
    n = recv(key->fd, ptr, count, 0);
    if(n > 0) {
        buffer_write_adv(d->rb, n);
        const enum hello_state st = hello_consume(d->rb, &d->parser, &error);
        if(hello_is_done(st, NULL)) {
            if(SELECTOR_SUCCESS == selector_set_interest_key(key, OP_WRITE)) {
                ret = hello_process(d);
            } else {
                ret = ERROR;
            }
        }
    } else {
        ret = ERROR;
    }

    return error ? ERROR : ret;
}

/** Escribe la respuesta del hello */
static unsigned
hello_write(struct selector_key *key) {
    struct hello_st *d = &ATTACHMENT(key)->client.hello;
    unsigned  ret      = HELLO_WRITE;
    uint8_t  *ptr;
    size_t    count;
    ssize_t   n;

    ptr = buffer_read_ptr(d->wb, &count);
    n = send(key->fd, ptr, count, MSG_NOSIGNAL);
    if(n == -1) {
        ret = ERROR;
    } else {
        buffer_read_adv(d->wb, n);
        if(!buffer_can_read(d->wb)) {
            if(d->method == SOCKS_HELLO_USERNAME_PASSWORD) {
                ret = AUTH_READ;
            } else {
                ret = REQUEST_READ;
            }
            if(SELECTOR_SUCCESS != selector_set_interest_key(key, OP_READ)) {
                ret = ERROR;
            }
        }
    }

    return ret;
}

////////////////////////////////////////////////////////////////////////////////
// AUTENTICACIÓN (RFC 1929)
////////////////////////////////////////////////////////////////////////////////

static void
auth_read_init(const unsigned state, struct selector_key *key) {
    (void) state;
    struct auth_st *d = &ATTACHMENT(key)->client.auth;

    d->rb = &ATTACHMENT(key)->read_buffer;
    d->wb = &ATTACHMENT(key)->write_buffer;
    auth_parser_init(&d->parser);
}

static void
auth_read_close(const unsigned state, struct selector_key *key) {
    (void) state;
    struct auth_st *d = &ATTACHMENT(key)->client.auth;
    auth_parser_close(&d->parser);
}

/** Valida las credenciales contra los usuarios configurados */
static bool
validate_credentials(const char *username, const char *password) {
    for(int i = 0; i < MAX_USERS; i++) {
        if(socks5_args.users[i].name != NULL) {
            if(strcmp(socks5_args.users[i].name, username) == 0 &&
               strcmp(socks5_args.users[i].pass, password) == 0) {
                return true;
            }
        }
    }
    return false;
}

/** Procesa la autenticación */
static unsigned
auth_process(struct auth_st *d, struct socks5 *s) {
    unsigned ret = AUTH_WRITE;

    bool valid = validate_credentials((char *)d->parser.username, 
                                       (char *)d->parser.password);
    
    d->status = valid ? 0x00 : 0x01;
    
    if(valid) {
        strncpy(s->username, (char *)d->parser.username, sizeof(s->username) - 1);
        metrics_auth_success();
    } else {
        metrics_auth_failed();
    }

    if(-1 == auth_marshall(d->wb, d->status)) {
        ret = ERROR;
    }

    return ret;
}

/** Lee credenciales de autenticación */
static unsigned
auth_read(struct selector_key *key) {
    struct auth_st *d = &ATTACHMENT(key)->client.auth;
    unsigned  ret     = AUTH_READ;
    bool      error   = false;
    uint8_t  *ptr;
    size_t    count;
    ssize_t   n;

    ptr = buffer_write_ptr(d->rb, &count);
    n = recv(key->fd, ptr, count, 0);
    if(n > 0) {
        buffer_write_adv(d->rb, n);
        const enum auth_state st = auth_consume(d->rb, &d->parser, &error);
        if(auth_is_done(st, NULL)) {
            if(SELECTOR_SUCCESS == selector_set_interest_key(key, OP_WRITE)) {
                ret = auth_process(d, ATTACHMENT(key));
            } else {
                ret = ERROR;
            }
        }
    } else {
        ret = ERROR;
    }

    return error ? ERROR : ret;
}

/** Escribe respuesta de autenticación */
static unsigned
auth_write(struct selector_key *key) {
    struct auth_st *d = &ATTACHMENT(key)->client.auth;
    unsigned  ret     = AUTH_WRITE;
    uint8_t  *ptr;
    size_t    count;
    ssize_t   n;

    ptr = buffer_read_ptr(d->wb, &count);
    n = send(key->fd, ptr, count, MSG_NOSIGNAL);
    if(n == -1) {
        ret = ERROR;
    } else {
        buffer_read_adv(d->wb, n);
        if(!buffer_can_read(d->wb)) {
            if(d->status == 0x00) {
                ret = REQUEST_READ;
                if(SELECTOR_SUCCESS != selector_set_interest_key(key, OP_READ)) {
                    ret = ERROR;
                }
            } else {
                ret = ERROR;
            }
        }
    }

    return ret;
}

////////////////////////////////////////////////////////////////////////////////
// REQUEST
////////////////////////////////////////////////////////////////////////////////

static void
request_read_init(const unsigned state, struct selector_key *key) {
    (void) state;
    struct request_st *d = &ATTACHMENT(key)->client.request;

    d->rb = &ATTACHMENT(key)->read_buffer;
    d->wb = &ATTACHMENT(key)->write_buffer;
    d->parser.request = &d->request;
    d->status = socks_status_general_SOCKS_server_failure;
    request_parser_init(&d->parser);
}

static void
request_read_close(const unsigned state, struct selector_key *key) {
    (void) state;
    struct request_st *d = &ATTACHMENT(key)->client.request;
    request_parser_close(&d->parser);
}

/** Datos para resolución DNS asíncrona */
struct dns_query {
    int            client_fd;
    fd_selector    selector;
    char           host[SOCKS_MAX_FQDN_LEN + 1];
    in_port_t      port;
    struct socks5 *socks5;
};

/** Hilo de resolución DNS */
static void *
dns_resolve_thread(void *data) {
    struct dns_query *q = (struct dns_query *)data;
    
    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags    = AI_PASSIVE,
        .ai_protocol = 0,
    };
    
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", ntohs(q->port));
    
    struct addrinfo *result = NULL;
    int gai_error = getaddrinfo(q->host, port_str, &hints, &result);
    
    if(gai_error == 0) {
        q->socks5->origin_resolution = result;
        q->socks5->origin_resolution_current = result;
    } else {
        q->socks5->origin_resolution = NULL;
    }
    
    // Notificar al selector
    selector_notify_block(q->selector, q->client_fd);
    
    free(q);
    return NULL;
}

/** Inicia resolución DNS asíncrona */
static unsigned
request_start_dns_resolution(struct selector_key *key) {
    struct request_st *d = &ATTACHMENT(key)->client.request;
    struct socks5 *s = ATTACHMENT(key);
    
    struct dns_query *q = malloc(sizeof(*q));
    if(q == NULL) {
        return ERROR;
    }
    
    q->client_fd = s->client_fd;
    q->selector  = key->s;
    q->port      = d->request.dest_port;
    q->socks5    = s;
    strncpy(q->host, d->request.dest_addr.fqdn, sizeof(q->host) - 1);
    
    pthread_t tid;
    if(pthread_create(&tid, NULL, dns_resolve_thread, q) != 0) {
        free(q);
        return ERROR;
    }
    pthread_detach(tid);
    
    return REQUEST_RESOLVING;
}

/** Conecta al servidor origen */
static unsigned
request_connect(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct request_st *d = &s->client.request;
    
    struct addrinfo *addr = s->origin_resolution_current;
    
    while(addr != NULL) {
        int fd = socket(addr->ai_family, SOCK_STREAM, 0);
        if(fd == -1) {
            addr = addr->ai_next;
            continue;
        }
        
        if(selector_fd_set_nio(fd) == -1) {
            close(fd);
            addr = addr->ai_next;
            continue;
        }
        
        int ret = connect(fd, addr->ai_addr, addr->ai_addrlen);
        if(ret == -1) {
            if(errno == EINPROGRESS) {
                // conexión en progreso
                s->origin_fd = fd;
                s->origin_resolution_current = addr->ai_next;
                
                // Registrar el fd del origen en el selector
                if(SELECTOR_SUCCESS != selector_register(key->s, fd, 
                    &socks5_handler, OP_WRITE, s)) {
                    close(fd);
                    s->origin_fd = -1;
                    addr = addr->ai_next;
                    continue;
                }
                
                s->references++;
                
                // Desactivar interés en el cliente mientras conectamos
                selector_set_interest_key(key, OP_NOOP);
                
                return REQUEST_CONNECTING;
            }
            close(fd);
            addr = addr->ai_next;
            continue;
        }
        
        // Conexión inmediata exitosa
        s->origin_fd = fd;
        memcpy(&s->origin_addr, addr->ai_addr, addr->ai_addrlen);
        s->origin_addr_len = addr->ai_addrlen;
        d->status = socks_status_succeeded;
        
        if(SELECTOR_SUCCESS != selector_register(key->s, fd,
            &socks5_handler, OP_READ, s)) {
            close(fd);
            s->origin_fd = -1;
            d->status = socks_status_general_SOCKS_server_failure;
        } else {
            s->references++;
        }
        
        return REQUEST_WRITE;
    }
    
    d->status = socks_status_host_unreachable;
    return REQUEST_WRITE;
}

/** Procesa el request del cliente */
static unsigned
request_process(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct request_st *d = &s->client.request;
    
    // Guardar destino para logging
    s->dest_port = ntohs(d->request.dest_port);
    switch(d->request.dest_addr_type) {
        case socks_req_addrtype_ipv4:
            inet_ntop(AF_INET, &d->request.dest_addr.ipv4, 
                      s->dest_addr_str, sizeof(s->dest_addr_str));
            break;
        case socks_req_addrtype_ipv6:
            inet_ntop(AF_INET6, &d->request.dest_addr.ipv6,
                      s->dest_addr_str, sizeof(s->dest_addr_str));
            break;
        case socks_req_addrtype_domain:
            strncpy(s->dest_addr_str, d->request.dest_addr.fqdn, 
                    sizeof(s->dest_addr_str) - 1);
            break;
        default:
            strncpy(s->dest_addr_str, "unknown", sizeof(s->dest_addr_str) - 1);
            break;
    }
    
    // Verificar que sea CONNECT
    if(d->request.cmd != socks_req_cmd_connect) {
        d->status = socks_status_command_not_supported;
        return REQUEST_WRITE;
    }
    
    // Preparar la resolución de direcciones
    switch(d->request.dest_addr_type) {
        case socks_req_addrtype_ipv4: {
            struct sockaddr_in *addr = (struct sockaddr_in *)&s->origin_addr;
            addr->sin_family = AF_INET;
            addr->sin_port = d->request.dest_port;
            memcpy(&addr->sin_addr, &d->request.dest_addr.ipv4, 4);
            s->origin_addr_len = sizeof(*addr);
            
            // Crear addrinfo manual
            struct addrinfo *ai = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
            if(ai == NULL) {
                d->status = socks_status_general_SOCKS_server_failure;
                return REQUEST_WRITE;
            }
            ai->ai_family = AF_INET;
            ai->ai_socktype = SOCK_STREAM;
            ai->ai_addrlen = sizeof(struct sockaddr_in);
            ai->ai_addr = (struct sockaddr *)(ai + 1);
            memcpy(ai->ai_addr, addr, sizeof(*addr));
            
            s->origin_resolution = ai;
            s->origin_resolution_current = ai;
            return request_connect(key);
        }
        
        case socks_req_addrtype_ipv6: {
            struct sockaddr_in6 *addr = (struct sockaddr_in6 *)&s->origin_addr;
            addr->sin6_family = AF_INET6;
            addr->sin6_port = d->request.dest_port;
            memcpy(&addr->sin6_addr, &d->request.dest_addr.ipv6, 16);
            s->origin_addr_len = sizeof(*addr);
            
            struct addrinfo *ai = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in6));
            if(ai == NULL) {
                d->status = socks_status_general_SOCKS_server_failure;
                return REQUEST_WRITE;
            }
            ai->ai_family = AF_INET6;
            ai->ai_socktype = SOCK_STREAM;
            ai->ai_addrlen = sizeof(struct sockaddr_in6);
            ai->ai_addr = (struct sockaddr *)(ai + 1);
            memcpy(ai->ai_addr, addr, sizeof(*addr));
            
            s->origin_resolution = ai;
            s->origin_resolution_current = ai;
            return request_connect(key);
        }
        
        case socks_req_addrtype_domain:
            // Necesita resolución DNS asíncrona
            if(SELECTOR_SUCCESS != selector_set_interest_key(key, OP_NOOP)) {
                return ERROR;
            }
            return request_start_dns_resolution(key);
            
        default:
            d->status = socks_status_address_type_not_supported;
            return REQUEST_WRITE;
    }
}

/** Lee el request del cliente */
static unsigned
request_read(struct selector_key *key) {
    struct request_st *d = &ATTACHMENT(key)->client.request;
    unsigned  ret     = REQUEST_READ;
    bool      error   = false;
    uint8_t  *ptr;
    size_t    count;
    ssize_t   n;

    ptr = buffer_write_ptr(d->rb, &count);
    n = recv(key->fd, ptr, count, 0);
    if(n > 0) {
        buffer_write_adv(d->rb, n);
        const enum request_state st = request_consume(d->rb, &d->parser, &error);
        if(request_is_done(st, NULL)) {
            ret = request_process(key);
        }
    } else {
        ret = ERROR;
    }

    return error ? ERROR : ret;
}

////////////////////////////////////////////////////////////////////////////////
// REQUEST_RESOLVING
////////////////////////////////////////////////////////////////////////////////

static void
request_resolving_init(const unsigned state, struct selector_key *key) {
    (void) state;
    (void) key;
}

/** Cuando termina la resolución DNS */
static unsigned
request_resolving_done(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct request_st *d = &s->client.request;
    
    if(s->origin_resolution == NULL) {
        d->status = socks_status_host_unreachable;
        selector_set_interest_key(key, OP_WRITE);
        return REQUEST_WRITE;
    }
    
    s->origin_resolution_current = s->origin_resolution;
    return request_connect(key);
}

////////////////////////////////////////////////////////////////////////////////
// REQUEST_CONNECTING
////////////////////////////////////////////////////////////////////////////////

static void
connecting_init(const unsigned state, struct selector_key *key) {
    (void) state;
    struct connecting *d = &ATTACHMENT(key)->orig.conn;
    d->fd = ATTACHMENT(key)->origin_fd;
    d->status = socks_status_succeeded;
}

/** Verifica si la conexión se completó */
static unsigned
connecting_write(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct request_st *d = &s->client.request;
    
    int error = 0;
    socklen_t len = sizeof(error);
    
    // Verificar si connect() tuvo éxito
    if(getsockopt(s->origin_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        error = errno;
    }
    
    if(error != 0) {
        // Falló, intentar siguiente dirección
        selector_unregister_fd(key->s, s->origin_fd);
        close(s->origin_fd);
        s->origin_fd = -1;
        s->references--;
        
        if(s->origin_resolution_current != NULL) {
            // Hay más direcciones para intentar
            struct selector_key client_key = {
                .s = key->s,
                .fd = s->client_fd,
                .data = s,
            };
            unsigned ret = request_connect(&client_key);
            if(ret == REQUEST_CONNECTING) {
                return REQUEST_CONNECTING;
            }
        }
        
        d->status = errno_to_socks(error);
        selector_set_interest(key->s, s->client_fd, OP_WRITE);
        return REQUEST_WRITE;
    }
    
    // Conexión exitosa
    d->status = socks_status_succeeded;
    metrics_connection_success();
    
    // Obtener la dirección local para la respuesta
    socklen_t addr_len = sizeof(s->origin_addr);
    getsockname(s->origin_fd, (struct sockaddr *)&s->origin_addr, &addr_len);
    s->origin_addr_len = addr_len;
    
    selector_set_interest(key->s, s->client_fd, OP_WRITE);
    selector_set_interest(key->s, s->origin_fd, OP_NOOP);
    
    return REQUEST_WRITE;
}

////////////////////////////////////////////////////////////////////////////////
// REQUEST_WRITE
////////////////////////////////////////////////////////////////////////////////

static void
request_write_init(const unsigned state, struct selector_key *key) {
    (void) state;
    struct socks5 *s = ATTACHMENT(key);
    struct request_st *d = &s->client.request;
    
    // Guardar status para logging
    s->last_status = d->status;
    
    // Armar respuesta
    enum socks_addr_type atyp = socks_req_addrtype_ipv4;
    union socks_addr addr;
    in_port_t port = 0;
    
    memset(&addr, 0, sizeof(addr));
    
    if(d->status == socks_status_succeeded && s->origin_fd != -1) {
        if(s->origin_addr.ss_family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&s->origin_addr;
            atyp = socks_req_addrtype_ipv4;
            memcpy(&addr.ipv4, &a->sin_addr, 4);
            port = a->sin_port;
        } else if(s->origin_addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&s->origin_addr;
            atyp = socks_req_addrtype_ipv6;
            memcpy(&addr.ipv6, &a->sin6_addr, 16);
            port = a->sin6_port;
        }
    }
    
    request_marshall(d->wb, d->status, atyp, &addr, port);
}

static unsigned
request_write(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct request_st *d = &s->client.request;
    unsigned  ret     = REQUEST_WRITE;
    uint8_t  *ptr;
    size_t    count;
    ssize_t   n;

    ptr = buffer_read_ptr(d->wb, &count);
    n = send(key->fd, ptr, count, MSG_NOSIGNAL);
    if(n == -1) {
        ret = ERROR;
    } else {
        buffer_read_adv(d->wb, n);
        if(!buffer_can_read(d->wb)) {
            if(d->status == socks_status_succeeded) {
                ret = COPY;
                // Activar lectura en ambos extremos
                selector_set_interest(key->s, s->client_fd, OP_READ);
                if(s->origin_fd != -1) {
                    selector_set_interest(key->s, s->origin_fd, OP_READ);
                }
            } else {
                metrics_connection_failed();
                ret = DONE;
            }
        }
    }

    return ret;
}

////////////////////////////////////////////////////////////////////////////////
// COPY
////////////////////////////////////////////////////////////////////////////////

static void
copy_init(const unsigned state, struct selector_key *key) {
    (void) state;
    struct socks5 *s = ATTACHMENT(key);
    
    struct copy *c_client = &s->client.copy;
    struct copy *c_origin = &s->orig.copy;
    
    c_client->fd     = &s->client_fd;
    c_client->rb     = &s->read_buffer;
    c_client->wb     = &s->write_buffer;
    c_client->duplex = OP_READ | OP_WRITE;
    c_client->other  = c_origin;
    
    c_origin->fd     = &s->origin_fd;
    c_origin->rb     = &s->write_buffer;
    c_origin->wb     = &s->read_buffer;
    c_origin->duplex = OP_READ | OP_WRITE;
    c_origin->other  = c_client;
}

static fd_interest
copy_compute_interests(fd_selector s, struct copy *d) {
    fd_interest ret = OP_NOOP;
    
    if((d->duplex & OP_READ) && buffer_can_write(d->rb)) {
        ret |= OP_READ;
    }
    if((d->duplex & OP_WRITE) && buffer_can_read(d->wb)) {
        ret |= OP_WRITE;
    }
    
    if(SELECTOR_SUCCESS != selector_set_interest(s, *d->fd, ret)) {
        abort();
    }
    
    return ret;
}

/** Copia datos entre los dos extremos */
static struct copy *
copy_ptr(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    
    if(key->fd == s->client_fd) {
        return &s->client.copy;
    } else {
        return &s->orig.copy;
    }
}

static unsigned
copy_read(struct selector_key *key) {
    struct copy *d = copy_ptr(key);
    
    if(d == NULL || *d->fd == -1) {
        return ERROR;
    }
    
    size_t   count;
    uint8_t *ptr = buffer_write_ptr(d->rb, &count);
    ssize_t  n   = recv(key->fd, ptr, count, 0);
    
    if(n <= 0) {
        // EOF o error: cerrar esta dirección
        shutdown(*d->fd, SHUT_RD);
        d->duplex = INTEREST_OFF(d->duplex, OP_READ);
        if(d->other->fd != NULL && *d->other->fd != -1) {
            shutdown(*d->other->fd, SHUT_WR);
            d->other->duplex = INTEREST_OFF(d->other->duplex, OP_WRITE);
        }
    } else {
        buffer_write_adv(d->rb, n);
        
        // Métricas
        struct socks5 *s = ATTACHMENT(key);
        if(key->fd == s->client_fd) {
            metrics_add_bytes_from_client(n);
        } else {
            metrics_add_bytes_from_origin(n);
            s->bytes_from_origin += n;  // Para logging
        }
    }
    
    copy_compute_interests(key->s, d);
    copy_compute_interests(key->s, d->other);
    
    if(d->duplex == OP_NOOP && d->other->duplex == OP_NOOP) {
        return DONE;
    }
    
    return COPY;
}

static unsigned
copy_write(struct selector_key *key) {
    struct copy *d = copy_ptr(key);
    
    if(d == NULL || *d->fd == -1) {
        return ERROR;
    }
    
    size_t   count;
    uint8_t *ptr = buffer_read_ptr(d->wb, &count);
    ssize_t  n   = send(key->fd, ptr, count, MSG_NOSIGNAL);
    
    if(n == -1) {
        shutdown(*d->fd, SHUT_WR);
        d->duplex = INTEREST_OFF(d->duplex, OP_WRITE);
        if(d->other->fd != NULL && *d->other->fd != -1) {
            shutdown(*d->other->fd, SHUT_RD);
            d->other->duplex = INTEREST_OFF(d->other->duplex, OP_READ);
        }
    } else {
        buffer_read_adv(d->wb, n);
        
        // Métricas
        struct socks5 *s = ATTACHMENT(key);
        if(key->fd == s->client_fd) {
            metrics_add_bytes_to_client(n);
        } else {
            metrics_add_bytes_to_origin(n);
            s->bytes_to_origin += n;  // Para logging
        }
    }
    
    copy_compute_interests(key->s, d);
    copy_compute_interests(key->s, d->other);
    
    if(d->duplex == OP_NOOP && d->other->duplex == OP_NOOP) {
        return DONE;
    }
    
    return COPY;
}

////////////////////////////////////////////////////////////////////////////////
// TABLA DE ESTADOS
////////////////////////////////////////////////////////////////////////////////

static const struct state_definition socks5_state_handlers[ERROR + 1] = {
    {
        .state          = HELLO_READ,
        .on_arrival     = hello_read_init,
        .on_departure   = hello_read_close,
        .on_read_ready  = hello_read,
    },
    {
        .state          = HELLO_WRITE,
        .on_write_ready = hello_write,
    },
    {
        .state          = AUTH_READ,
        .on_arrival     = auth_read_init,
        .on_departure   = auth_read_close,
        .on_read_ready  = auth_read,
    },
    {
        .state          = AUTH_WRITE,
        .on_write_ready = auth_write,
    },
    {
        .state          = REQUEST_READ,
        .on_arrival     = request_read_init,
        .on_departure   = request_read_close,
        .on_read_ready  = request_read,
    },
    {
        .state          = REQUEST_RESOLVING,
        .on_arrival     = request_resolving_init,
        .on_block_ready = request_resolving_done,
    },
    {
        .state          = REQUEST_CONNECTING,
        .on_arrival     = connecting_init,
        .on_write_ready = connecting_write,
    },
    {
        .state          = REQUEST_WRITE,
        .on_arrival     = request_write_init,
        .on_write_ready = request_write,
    },
    {
        .state          = COPY,
        .on_arrival     = copy_init,
        .on_read_ready  = copy_read,
        .on_write_ready = copy_write,
    },
    {
        .state          = DONE,
    },
    {
        .state          = ERROR,
    },
};

////////////////////////////////////////////////////////////////////////////////
// HANDLERS TOP-LEVEL
////////////////////////////////////////////////////////////////////////////////

void
socksv5_passive_accept(struct selector_key *key) {
    struct sockaddr_storage       client_addr;
    socklen_t                     client_addr_len = sizeof(client_addr);
    struct socks5                *state           = NULL;

    const int client = accept(key->fd, (struct sockaddr*) &client_addr,
                                                          &client_addr_len);
    if(client == -1) {
        goto fail;
    }
    if(selector_fd_set_nio(client) == -1) {
        goto fail;
    }
    state = socks5_new(client);
    if(state == NULL) {
        goto fail;
    }
    memcpy(&state->client_addr, &client_addr, client_addr_len);
    state->client_addr_len = client_addr_len;

    if(SELECTOR_SUCCESS != selector_register(key->s, client, &socks5_handler,
                                              OP_READ, state)) {
        goto fail;
    }
    
    metrics_connection_opened();
    
    // Log de nueva conexión
    char buff[SOCKADDR_TO_HUMAN_MIN];
    fprintf(stdout, "Connection from %s\n", 
            sockaddr_to_human(buff, sizeof(buff), (struct sockaddr *)&client_addr));
    
    return;
    
fail:
    if(client != -1) {
        close(client);
    }
    socks5_destroy(state);
}

static void
socksv5_read(struct selector_key *key) {
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const enum socks_v5state st = stm_handler_read(stm, key);

    if(ERROR == st || DONE == st) {
        socksv5_done(key);
    }
}

static void
socksv5_write(struct selector_key *key) {
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const enum socks_v5state st = stm_handler_write(stm, key);

    if(ERROR == st || DONE == st) {
        socksv5_done(key);
    }
}

static void
socksv5_block(struct selector_key *key) {
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const enum socks_v5state st = stm_handler_block(stm, key);

    if(ERROR == st || DONE == st) {
        socksv5_done(key);
    }
}

static void
socksv5_close(struct selector_key *key) {
    socks5_destroy(ATTACHMENT(key));
}

static void
socksv5_done(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    
    // Registrar acceso antes de cerrar
    if(s->dest_addr_str[0] != '\0') {
        log_access(
            s->username[0] ? s->username : NULL,
            (struct sockaddr *)&s->client_addr,
            s->dest_addr_str,
            s->dest_port,
            s->last_status,
            s->bytes_to_origin,
            s->bytes_from_origin
        );
    }
    
    const int fds[] = {
        s->client_fd,
        s->origin_fd,
    };
    
    for(unsigned i = 0; i < N(fds); i++) {
        if(fds[i] != -1) {
            if(SELECTOR_SUCCESS != selector_unregister_fd(key->s, fds[i])) {
                abort();
            }
            close(fds[i]);
        }
    }
    
    metrics_connection_closed();
}

unsigned
socksv5_get_connection_count(void) {
    return metrics_get()->current_connections;
}
