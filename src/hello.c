/**
 * hello.c - Parser del mensaje de negociación HELLO de SOCKS5 (RFC 1928)
 *
 * Maneja lecturas parciales de forma robusta.
 */
#include <stdio.h>
#include <stdlib.h>

#include "hello.h"

#define SOCKS_VERSION 0x05

void 
hello_parser_init(struct hello_parser *p) {
    p->state     = hello_version;
    p->remaining = 0;
}

enum hello_state 
hello_parser_feed(struct hello_parser *p, uint8_t b) {
    switch(p->state) {
        case hello_version:
            if(b == SOCKS_VERSION) {
                p->state = hello_nmethods;
            } else {
                p->state = hello_error_unsupported_version;
            }
            break;
            
        case hello_nmethods:
            if(b > 0) {
                p->remaining = b;
                p->state = hello_methods;
            } else {
                // nmethods = 0 es inválido
                p->state = hello_error;
            }
            break;
            
        case hello_methods:
            if(p->on_authentication_method != NULL) {
                p->on_authentication_method(p, b);
            }
            p->remaining--;
            if(p->remaining == 0) {
                p->state = hello_done;
            }
            break;
            
        case hello_done:
        case hello_error:
        case hello_error_unsupported_version:
            // ya terminó, no deberíamos seguir recibiendo bytes
            break;
    }
    
    return p->state;
}

enum hello_state 
hello_consume(buffer *b, struct hello_parser *p, bool *errored) {
    enum hello_state state = p->state;
    
    while(buffer_can_read(b)) {
        const uint8_t byte = buffer_read(b);
        state = hello_parser_feed(p, byte);
        if(hello_is_done(state, errored)) {
            break;
        }
    }
    
    return state;
}

bool 
hello_is_done(const enum hello_state state, bool *errored) {
    bool ret = false;
    
    switch(state) {
        case hello_done:
            ret = true;
            break;
        case hello_error:
        case hello_error_unsupported_version:
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
hello_marshall(buffer *b, const uint8_t method) {
    size_t n;
    uint8_t *buf = buffer_write_ptr(b, &n);
    
    if(n < 2) {
        return -1;
    }
    
    buf[0] = SOCKS_VERSION;
    buf[1] = method;
    buffer_write_adv(b, 2);
    
    return 2;
}

void 
hello_parser_close(struct hello_parser *p) {
    // no hay recursos dinámicos que liberar
    (void) p;
}

const char *
hello_error_description(const struct hello_parser *p) {
    const char *ret;
    
    switch(p->state) {
        case hello_error_unsupported_version:
            ret = "unsupported SOCKS version";
            break;
        case hello_error:
            ret = "invalid hello message";
            break;
        default:
            ret = "";
            break;
    }
    
    return ret;
}

