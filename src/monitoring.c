/**
 * monitoring.c - Servidor de Monitoreo y Configuración
 *
 * Implementa un protocolo binario simple para monitorear y configurar
 * el servidor SOCKSv5 en tiempo de ejecución.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "monitoring.h"
#include "buffer.h"
#include "metrics.h"
#include "args.h"
#include "netutils.h"

#define BUFFER_SIZE 4096

extern struct socks5args socks5_args;

////////////////////////////////////////////////////////////////////////////////
// ESTADOS DEL MONITOR
////////////////////////////////////////////////////////////////////////////////

enum monitoring_state {
    MON_READ_HEADER,
    MON_READ_DATA,
    MON_WRITE,
    MON_DONE,
    MON_ERROR,
};

////////////////////////////////////////////////////////////////////////////////
// ESTRUCTURA DE CONEXIÓN
////////////////////////////////////////////////////////////////////////////////

struct monitoring_conn {
    int                     fd;
    struct sockaddr_storage client_addr;
    socklen_t               client_addr_len;
    
    enum monitoring_state   state;
    
    // Buffers
    uint8_t raw_read[BUFFER_SIZE];
    uint8_t raw_write[BUFFER_SIZE];
    buffer  read_buffer;
    buffer  write_buffer;
    
    // Mensaje recibido
    uint8_t  version;
    uint8_t  cmd;
    uint16_t data_len;
    uint16_t data_read;
    uint8_t  data[BUFFER_SIZE];
    
    // Para pool
    struct monitoring_conn *next;
};

// Pool simple
static struct monitoring_conn *pool = NULL;
static const unsigned max_pool = 10;
static unsigned pool_size = 0;

static struct monitoring_conn *
monitoring_new(int fd) {
    struct monitoring_conn *ret;
    
    if(pool != NULL) {
        ret = pool;
        pool = pool->next;
        pool_size--;
    } else {
        ret = malloc(sizeof(*ret));
    }
    
    if(ret == NULL) {
        return NULL;
    }
    
    memset(ret, 0, sizeof(*ret));
    ret->fd = fd;
    ret->state = MON_READ_HEADER;
    
    buffer_init(&ret->read_buffer, sizeof(ret->raw_read), ret->raw_read);
    buffer_init(&ret->write_buffer, sizeof(ret->raw_write), ret->raw_write);
    
    return ret;
}

static void
monitoring_free(struct monitoring_conn *c) {
    if(c == NULL) {
        return;
    }
    
    if(pool_size < max_pool) {
        c->next = pool;
        pool = c;
        pool_size++;
    } else {
        free(c);
    }
}

#define MON_ATTACHMENT(key) ((struct monitoring_conn *)(key)->data)

////////////////////////////////////////////////////////////////////////////////
// HANDLERS
////////////////////////////////////////////////////////////////////////////////

static void monitoring_read(struct selector_key *key);
static void monitoring_write(struct selector_key *key);
static void monitoring_close(struct selector_key *key);

static const struct fd_handler monitoring_handler = {
    .handle_read  = monitoring_read,
    .handle_write = monitoring_write,
    .handle_close = monitoring_close,
    .handle_block = NULL,
};

////////////////////////////////////////////////////////////////////////////////
// PROCESAMIENTO DE COMANDOS
////////////////////////////////////////////////////////////////////////////////

/** Escribe métricas en el buffer de respuesta */
static void
write_metrics_response(struct monitoring_conn *c) {
    struct socks5_metrics *m = metrics_get();
    
    // Formato de respuesta de métricas:
    // historical_connections (8 bytes)
    // current_connections (8 bytes)
    // bytes_transferred (8 bytes)
    
    size_t n;
    uint8_t *buf = buffer_write_ptr(&c->write_buffer, &n);
    
    // Header de respuesta
    buf[0] = MONITORING_VERSION;
    buf[1] = MONITORING_STATUS_OK;
    
    // Longitud de datos: 8 * 6 = 48 bytes de métricas
    uint16_t len = 48;
    buf[2] = (len >> 8) & 0xFF;
    buf[3] = len & 0xFF;
    
    // Datos
    uint64_t hist = m->historical_connections;
    uint64_t curr = m->current_connections;
    uint64_t bytes = m->bytes_transferred;
    uint64_t succ = m->successful_connections;
    uint64_t fail = m->failed_connections;
    uint64_t bytes_client = m->bytes_from_clients + m->bytes_to_clients;
    
    // Network byte order (big-endian)
    for(int i = 7; i >= 0; i--) {
        buf[4 + (7 - i)] = (hist >> (i * 8)) & 0xFF;
    }
    for(int i = 7; i >= 0; i--) {
        buf[12 + (7 - i)] = (curr >> (i * 8)) & 0xFF;
    }
    for(int i = 7; i >= 0; i--) {
        buf[20 + (7 - i)] = (bytes >> (i * 8)) & 0xFF;
    }
    for(int i = 7; i >= 0; i--) {
        buf[28 + (7 - i)] = (succ >> (i * 8)) & 0xFF;
    }
    for(int i = 7; i >= 0; i--) {
        buf[36 + (7 - i)] = (fail >> (i * 8)) & 0xFF;
    }
    for(int i = 7; i >= 0; i--) {
        buf[44 + (7 - i)] = (bytes_client >> (i * 8)) & 0xFF;
    }
    
    buffer_write_adv(&c->write_buffer, 4 + len);
}

/** Lista usuarios configurados */
static void
write_users_response(struct monitoring_conn *c) {
    size_t n;
    uint8_t *buf = buffer_write_ptr(&c->write_buffer, &n);
    
    buf[0] = MONITORING_VERSION;
    buf[1] = MONITORING_STATUS_OK;
    
    // Calcular longitud
    size_t data_len = 0;
    int user_count = 0;
    
    for(int i = 0; i < MAX_USERS; i++) {
        if(socks5_args.users[i].name != NULL) {
            // 1 byte len + nombre
            data_len += 1 + strlen(socks5_args.users[i].name);
            user_count++;
        }
    }
    
    // 1 byte para cantidad de usuarios
    data_len += 1;
    
    buf[2] = (data_len >> 8) & 0xFF;
    buf[3] = data_len & 0xFF;
    
    // Cantidad de usuarios
    buf[4] = user_count;
    
    size_t offset = 5;
    for(int i = 0; i < MAX_USERS; i++) {
        if(socks5_args.users[i].name != NULL) {
            size_t ulen = strlen(socks5_args.users[i].name);
            buf[offset++] = ulen;
            memcpy(buf + offset, socks5_args.users[i].name, ulen);
            offset += ulen;
        }
    }
    
    buffer_write_adv(&c->write_buffer, 4 + data_len);
}

/** Agrega un usuario */
static void
handle_add_user(struct monitoring_conn *c) {
    size_t n;
    uint8_t *buf = buffer_write_ptr(&c->write_buffer, &n);
    
    buf[0] = MONITORING_VERSION;
    
    // Parsear datos: ulen(1) + user + plen(1) + pass
    if(c->data_len < 2) {
        buf[1] = MONITORING_STATUS_ERROR;
        buf[2] = 0;
        buf[3] = 0;
        buffer_write_adv(&c->write_buffer, 4);
        return;
    }
    
    uint8_t ulen = c->data[0];
    if(c->data_len < 2 + ulen) {
        buf[1] = MONITORING_STATUS_ERROR;
        buf[2] = 0;
        buf[3] = 0;
        buffer_write_adv(&c->write_buffer, 4);
        return;
    }
    
    char username[256];
    memcpy(username, c->data + 1, ulen);
    username[ulen] = '\0';
    
    uint8_t plen = c->data[1 + ulen];
    char password[256];
    memcpy(password, c->data + 2 + ulen, plen);
    password[plen] = '\0';
    
    // Verificar si ya existe
    for(int i = 0; i < MAX_USERS; i++) {
        if(socks5_args.users[i].name != NULL &&
           strcmp(socks5_args.users[i].name, username) == 0) {
            buf[1] = MONITORING_STATUS_USER_EXISTS;
            buf[2] = 0;
            buf[3] = 0;
            buffer_write_adv(&c->write_buffer, 4);
            return;
        }
    }
    
    // Buscar slot libre
    for(int i = 0; i < MAX_USERS; i++) {
        if(socks5_args.users[i].name == NULL) {
            socks5_args.users[i].name = strdup(username);
            socks5_args.users[i].pass = strdup(password);
            
            buf[1] = MONITORING_STATUS_OK;
            buf[2] = 0;
            buf[3] = 0;
            buffer_write_adv(&c->write_buffer, 4);
            
            fprintf(stdout, "[MONITOR] User added: %s\n", username);
            return;
        }
    }
    
    buf[1] = MONITORING_STATUS_USER_LIMIT;
    buf[2] = 0;
    buf[3] = 0;
    buffer_write_adv(&c->write_buffer, 4);
}

/** Elimina un usuario */
static void
handle_remove_user(struct monitoring_conn *c) {
    size_t n;
    uint8_t *buf = buffer_write_ptr(&c->write_buffer, &n);
    
    buf[0] = MONITORING_VERSION;
    
    if(c->data_len < 1) {
        buf[1] = MONITORING_STATUS_ERROR;
        buf[2] = 0;
        buf[3] = 0;
        buffer_write_adv(&c->write_buffer, 4);
        return;
    }
    
    uint8_t ulen = c->data[0];
    char username[256];
    memcpy(username, c->data + 1, ulen);
    username[ulen] = '\0';
    
    for(int i = 0; i < MAX_USERS; i++) {
        if(socks5_args.users[i].name != NULL &&
           strcmp(socks5_args.users[i].name, username) == 0) {
            free(socks5_args.users[i].name);
            free(socks5_args.users[i].pass);
            socks5_args.users[i].name = NULL;
            socks5_args.users[i].pass = NULL;
            
            buf[1] = MONITORING_STATUS_OK;
            buf[2] = 0;
            buf[3] = 0;
            buffer_write_adv(&c->write_buffer, 4);
            
            fprintf(stdout, "[MONITOR] User removed: %s\n", username);
            return;
        }
    }
    
    buf[1] = MONITORING_STATUS_USER_NOT_FOUND;
    buf[2] = 0;
    buf[3] = 0;
    buffer_write_adv(&c->write_buffer, 4);
}

/** Toggle del disector */
static void
handle_toggle_disector(struct monitoring_conn *c) {
    size_t n;
    uint8_t *buf = buffer_write_ptr(&c->write_buffer, &n);
    
    socks5_args.disectors_enabled = !socks5_args.disectors_enabled;
    
    buf[0] = MONITORING_VERSION;
    buf[1] = MONITORING_STATUS_OK;
    buf[2] = 0;
    buf[3] = 1;
    buf[4] = socks5_args.disectors_enabled ? 1 : 0;
    buffer_write_adv(&c->write_buffer, 5);
    
    fprintf(stdout, "[MONITOR] Disector %s\n", 
            socks5_args.disectors_enabled ? "enabled" : "disabled");
}

/** Procesa el comando recibido */
static void
process_command(struct monitoring_conn *c) {
    switch(c->cmd) {
        case MONITORING_CMD_GET_METRICS:
            write_metrics_response(c);
            break;
        case MONITORING_CMD_LIST_USERS:
            write_users_response(c);
            break;
        case MONITORING_CMD_ADD_USER:
            handle_add_user(c);
            break;
        case MONITORING_CMD_REMOVE_USER:
            handle_remove_user(c);
            break;
        case MONITORING_CMD_TOGGLE_DISECTOR:
            handle_toggle_disector(c);
            break;
        default: {
            size_t n;
            uint8_t *buf = buffer_write_ptr(&c->write_buffer, &n);
            buf[0] = MONITORING_VERSION;
            buf[1] = MONITORING_STATUS_CMD_NOT_SUPPORTED;
            buf[2] = 0;
            buf[3] = 0;
            buffer_write_adv(&c->write_buffer, 4);
            break;
        }
    }
    
    c->state = MON_WRITE;
}

////////////////////////////////////////////////////////////////////////////////
// READ/WRITE HANDLERS
////////////////////////////////////////////////////////////////////////////////

static void
monitoring_read(struct selector_key *key) {
    struct monitoring_conn *c = MON_ATTACHMENT(key);
    
    uint8_t *ptr;
    size_t count;
    ssize_t n;
    
    ptr = buffer_write_ptr(&c->read_buffer, &count);
    n = recv(c->fd, ptr, count, 0);
    
    if(n <= 0) {
        c->state = MON_ERROR;
        selector_unregister_fd(key->s, c->fd);
        close(c->fd);
        monitoring_free(c);
        return;
    }
    
    buffer_write_adv(&c->read_buffer, n);
    
    // Procesar header
    if(c->state == MON_READ_HEADER) {
        if(buffer_can_read(&c->read_buffer) && 
           (c->read_buffer.write - c->read_buffer.read) >= 4) {
            c->version = buffer_read(&c->read_buffer);
            c->cmd = buffer_read(&c->read_buffer);
            uint8_t len_hi = buffer_read(&c->read_buffer);
            uint8_t len_lo = buffer_read(&c->read_buffer);
            c->data_len = (len_hi << 8) | len_lo;
            c->data_read = 0;
            
            if(c->version != MONITORING_VERSION) {
                c->state = MON_ERROR;
                selector_unregister_fd(key->s, c->fd);
                close(c->fd);
                monitoring_free(c);
                return;
            }
            
            if(c->data_len == 0) {
                process_command(c);
                selector_set_interest_key(key, OP_WRITE);
            } else {
                c->state = MON_READ_DATA;
            }
        }
    }
    
    // Leer datos adicionales
    if(c->state == MON_READ_DATA) {
        while(buffer_can_read(&c->read_buffer) && c->data_read < c->data_len) {
            c->data[c->data_read++] = buffer_read(&c->read_buffer);
        }
        
        if(c->data_read >= c->data_len) {
            process_command(c);
            selector_set_interest_key(key, OP_WRITE);
        }
    }
}

static void
monitoring_write(struct selector_key *key) {
    struct monitoring_conn *c = MON_ATTACHMENT(key);
    
    uint8_t *ptr;
    size_t count;
    ssize_t n;
    
    ptr = buffer_read_ptr(&c->write_buffer, &count);
    n = send(c->fd, ptr, count, MSG_NOSIGNAL);
    
    if(n == -1) {
        c->state = MON_ERROR;
        selector_unregister_fd(key->s, c->fd);
        close(c->fd);
        monitoring_free(c);
        return;
    }
    
    buffer_read_adv(&c->write_buffer, n);
    
    if(!buffer_can_read(&c->write_buffer)) {
        // Respuesta enviada, preparar para siguiente comando
        c->state = MON_READ_HEADER;
        buffer_reset(&c->read_buffer);
        buffer_reset(&c->write_buffer);
        selector_set_interest_key(key, OP_READ);
    }
}

static void
monitoring_close(struct selector_key *key) {
    struct monitoring_conn *c = MON_ATTACHMENT(key);
    monitoring_free(c);
}

////////////////////////////////////////////////////////////////////////////////
// ACCEPT
////////////////////////////////////////////////////////////////////////////////

void
monitoring_passive_accept(struct selector_key *key) {
    struct sockaddr_storage client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    int client = accept(key->fd, (struct sockaddr *)&client_addr, &client_addr_len);
    if(client == -1) {
        return;
    }
    
    if(selector_fd_set_nio(client) == -1) {
        close(client);
        return;
    }
    
    struct monitoring_conn *state = monitoring_new(client);
    if(state == NULL) {
        close(client);
        return;
    }
    
    memcpy(&state->client_addr, &client_addr, client_addr_len);
    state->client_addr_len = client_addr_len;
    
    if(SELECTOR_SUCCESS != selector_register(key->s, client, &monitoring_handler,
                                              OP_READ, state)) {
        close(client);
        monitoring_free(state);
        return;
    }
    
    char buff[SOCKADDR_TO_HUMAN_MIN];
    fprintf(stdout, "[MONITOR] Connection from %s\n",
            sockaddr_to_human(buff, sizeof(buff), (struct sockaddr *)&client_addr));
}

void
monitoring_destroy(void) {
    struct monitoring_conn *next;
    while(pool != NULL) {
        next = pool->next;
        free(pool);
        pool = next;
    }
    pool_size = 0;
}

