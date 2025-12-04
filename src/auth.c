/**
 * auth.c - Parser de autenticación Usuario/Contraseña RFC 1929
 *
 * Maneja lecturas parciales de forma robusta.
 */
#include <string.h>
#include <stdlib.h>

#include "auth.h"

void 
auth_parser_init(struct auth_parser *p) {
    memset(p, 0, sizeof(*p));
    p->state = auth_version;
}

enum auth_state 
auth_parser_feed(struct auth_parser *p, uint8_t b) {
    switch(p->state) {
        case auth_version:
            if(b == AUTH_VERSION) {
                p->state = auth_ulen;
            } else {
                p->state = auth_error_unsupported_version;
            }
            break;
            
        case auth_ulen:
            if(b > 0) {
                // uint8_t siempre es <= 255 = AUTH_MAX_USERNAME_LEN
                p->remaining    = b;
                p->username_len = b;
                p->idx          = 0;
                p->state        = auth_uname;
            } else {
                // username vacío es inválido según RFC 1929
                p->state = auth_error;
            }
            break;
            
        case auth_uname:
            p->username[p->idx++] = b;
            p->remaining--;
            if(p->remaining == 0) {
                p->username[p->idx] = '\0';
                p->state = auth_plen;
            }
            break;
            
        case auth_plen:
            // uint8_t siempre es <= 255 = AUTH_MAX_PASSWORD_LEN
            if(b == 0) {
                // password vacío está permitido
                p->password_len = 0;
                p->password[0] = '\0';
                p->state = auth_done;
            } else {
                p->remaining     = b;
                p->password_len  = b;
                p->idx           = 0;
                p->state         = auth_passwd;
            }
            break;
            
        case auth_passwd:
            p->password[p->idx++] = b;
            p->remaining--;
            if(p->remaining == 0) {
                p->password[p->idx] = '\0';
                p->state = auth_done;
            }
            break;
            
        case auth_done:
        case auth_error:
        case auth_error_unsupported_version:
            // ya terminó
            break;
    }
    
    return p->state;
}

enum auth_state 
auth_consume(buffer *b, struct auth_parser *p, bool *errored) {
    enum auth_state state = p->state;
    
    while(buffer_can_read(b)) {
        const uint8_t byte = buffer_read(b);
        state = auth_parser_feed(p, byte);
        if(auth_is_done(state, errored)) {
            break;
        }
    }
    
    return state;
}

bool 
auth_is_done(const enum auth_state state, bool *errored) {
    bool ret = false;
    
    switch(state) {
        case auth_done:
            ret = true;
            break;
        case auth_error:
        case auth_error_unsupported_version:
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
auth_marshall(buffer *b, const uint8_t status) {
    size_t n;
    uint8_t *buf = buffer_write_ptr(b, &n);
    
    if(n < 2) {
        return -1;
    }
    
    buf[0] = AUTH_VERSION;
    buf[1] = status;
    buffer_write_adv(b, 2);
    
    return 2;
}

void 
auth_parser_close(struct auth_parser *p) {
    // limpiamos las credenciales de memoria por seguridad
    memset(p->username, 0, sizeof(p->username));
    memset(p->password, 0, sizeof(p->password));
}

