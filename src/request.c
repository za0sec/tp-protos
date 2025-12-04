/**
 * request.c - Parser del mensaje REQUEST de SOCKS5 (RFC 1928)
 *
 * Maneja lecturas parciales de forma robusta.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>

#include "request.h"

#define SOCKS_VERSION 0x05

void 
request_parser_init(struct request_parser *p) {
    p->state     = request_version;
    p->remaining = 0;
    p->addr_idx  = 0;
    memset(p->request, 0, sizeof(*(p->request)));
}

enum request_state 
request_parser_feed(struct request_parser *p, uint8_t b) {
    switch(p->state) {
        case request_version:
            if(b == SOCKS_VERSION) {
                p->state = request_cmd;
            } else {
                p->state = request_error_unsupported_version;
            }
            break;
            
        case request_cmd:
            switch(b) {
                case socks_req_cmd_connect:
                    p->request->cmd = b;
                    p->state = request_rsv;
                    break;
                case socks_req_cmd_bind:
                case socks_req_cmd_udp_associate:
                    // no soportados
                    p->state = request_error_unsupported_cmd;
                    break;
                default:
                    p->state = request_error_unsupported_cmd;
                    break;
            }
            break;
            
        case request_rsv:
            // campo reservado, debe ser 0x00, pero lo ignoramos
            p->state = request_atyp;
            break;
            
        case request_atyp:
            p->request->dest_addr_type = b;
            switch(b) {
                case socks_req_addrtype_ipv4:
                    p->remaining = 4;  // IPv4 = 4 bytes
                    p->addr_idx  = 0;
                    p->state     = request_dstaddr;
                    break;
                case socks_req_addrtype_domain:
                    p->state = request_dstaddr_fqdn_len;
                    break;
                case socks_req_addrtype_ipv6:
                    p->remaining = 16;  // IPv6 = 16 bytes
                    p->addr_idx  = 0;
                    p->state     = request_dstaddr;
                    break;
                default:
                    p->state = request_error_unsupported_atyp;
                    break;
            }
            break;
            
        case request_dstaddr_fqdn_len:
            // uint8_t siempre es <= 255 = SOCKS_MAX_FQDN_LEN
            if(b > 0) {
                p->remaining = b;
                p->addr_idx  = 0;
                p->state     = request_dstaddr;
            } else {
                p->state = request_error;
            }
            break;
            
        case request_dstaddr:
            switch(p->request->dest_addr_type) {
                case socks_req_addrtype_ipv4:
                    ((uint8_t *)&p->request->dest_addr.ipv4)[p->addr_idx++] = b;
                    break;
                case socks_req_addrtype_ipv6:
                    p->request->dest_addr.ipv6.s6_addr[p->addr_idx++] = b;
                    break;
                case socks_req_addrtype_domain:
                    p->request->dest_addr.fqdn[p->addr_idx++] = b;
                    break;
            }
            p->remaining--;
            if(p->remaining == 0) {
                // si era FQDN, agregar null terminator
                if(p->request->dest_addr_type == socks_req_addrtype_domain) {
                    p->request->dest_addr.fqdn[p->addr_idx] = '\0';
                }
                p->remaining = 2;  // puerto = 2 bytes
                p->addr_idx  = 0;
                p->state     = request_dstport;
            }
            break;
            
        case request_dstport:
            // puerto en network byte order (big-endian)
            ((uint8_t *)&p->request->dest_port)[p->addr_idx++] = b;
            p->remaining--;
            if(p->remaining == 0) {
                p->state = request_done;
            }
            break;
            
        case request_done:
        case request_error:
        case request_error_unsupported_version:
        case request_error_unsupported_atyp:
        case request_error_unsupported_cmd:
            // ya terminó
            break;
    }
    
    return p->state;
}

enum request_state 
request_consume(buffer *b, struct request_parser *p, bool *errored) {
    enum request_state state = p->state;
    
    while(buffer_can_read(b)) {
        const uint8_t byte = buffer_read(b);
        state = request_parser_feed(p, byte);
        if(request_is_done(state, errored)) {
            break;
        }
    }
    
    return state;
}

bool 
request_is_done(const enum request_state state, bool *errored) {
    bool ret = false;
    
    switch(state) {
        case request_done:
            ret = true;
            break;
        case request_error:
        case request_error_unsupported_version:
        case request_error_unsupported_atyp:
        case request_error_unsupported_cmd:
            if(errored != NULL) {
                *errored = true;
            }
            ret = true;
            break;
        default:
            ret = false;
            break;
    }
    
    return ret;
}

int 
request_marshall(buffer *b, 
                 const enum socks_reply_status status,
                 const enum socks_addr_type atyp,
                 const union socks_addr *addr,
                 const in_port_t port) {
    size_t n;
    uint8_t *buf = buffer_write_ptr(b, &n);
    
    size_t addr_len;
    switch(atyp) {
        case socks_req_addrtype_ipv4:
            addr_len = 4;
            break;
        case socks_req_addrtype_ipv6:
            addr_len = 16;
            break;
        case socks_req_addrtype_domain:
            // no deberíamos llegar aquí en una respuesta normal
            addr_len = 1;
            break;
        default:
            addr_len = 4;  // fallback a IPv4
            break;
    }
    
    // VER(1) + REP(1) + RSV(1) + ATYP(1) + ADDR(variable) + PORT(2)
    size_t required = 4 + addr_len + 2;
    
    if(n < required) {
        return -1;
    }
    
    buf[0] = SOCKS_VERSION;
    buf[1] = status;
    buf[2] = 0x00;  // RSV
    buf[3] = atyp;
    
    // Copiar dirección
    if(addr != NULL) {
        switch(atyp) {
            case socks_req_addrtype_ipv4:
                memcpy(buf + 4, &addr->ipv4, 4);
                break;
            case socks_req_addrtype_ipv6:
                memcpy(buf + 4, &addr->ipv6, 16);
                break;
            default:
                memset(buf + 4, 0, addr_len);
                break;
        }
    } else {
        memset(buf + 4, 0, addr_len);
    }
    
    // Copiar puerto
    memcpy(buf + 4 + addr_len, &port, 2);
    
    buffer_write_adv(b, required);
    
    return required;
}

void 
request_parser_close(struct request_parser *p) {
    (void) p;
}

enum socks_reply_status 
errno_to_socks(const int e) {
    enum socks_reply_status ret;
    
    switch(e) {
        case 0:
            ret = socks_status_succeeded;
            break;
        case ECONNREFUSED:
            ret = socks_status_connection_refused;
            break;
        case EHOSTUNREACH:
            ret = socks_status_host_unreachable;
            break;
        case ENETUNREACH:
            ret = socks_status_network_unreachable;
            break;
        case ETIMEDOUT:
            ret = socks_status_ttl_expired;
            break;
        default:
            ret = socks_status_general_SOCKS_server_failure;
            break;
    }
    
    return ret;
}

const char *
request_error_description(const struct request_parser *p) {
    const char *ret;
    
    switch(p->state) {
        case request_error_unsupported_version:
            ret = "unsupported SOCKS version";
            break;
        case request_error_unsupported_cmd:
            ret = "unsupported command";
            break;
        case request_error_unsupported_atyp:
            ret = "unsupported address type";
            break;
        case request_error:
            ret = "invalid request";
            break;
        default:
            ret = "";
            break;
    }
    
    return ret;
}

