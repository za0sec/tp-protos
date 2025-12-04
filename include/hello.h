#ifndef HELLO_H_Ds3wbvgeUHWkGm9U9x7vZKJ30fM
#define HELLO_H_Ds3wbvgeUHWkGm9U9x7vZKJ30fM

#include <stdint.h>
#include <stdbool.h>

#include "buffer.h"

/**
 * El mensaje de negociación inicial del cliente (RFC 1928):
 *
 *                    +----+----------+----------+
 *                    |VER | NMETHODS | METHODS  |
 *                    +----+----------+----------+
 *                    | 1  |    1     | 1 to 255 |
 *                    +----+----------+----------+
 *
 * VER: versión del protocolo = 0x05
 * NMETHODS: número de métodos de autenticación soportados
 * METHODS: lista de métodos de autenticación
 */

/** Estados del parser de hello */
enum hello_state {
    hello_version,
    hello_nmethods,
    hello_methods,
    hello_done,
    hello_error_unsupported_version,
    hello_error,
};

/** Métodos de autenticación SOCKS5 */
#define SOCKS_HELLO_NOAUTHENTICATION_REQUIRED  0x00
#define SOCKS_HELLO_GSSAPI                     0x01
#define SOCKS_HELLO_USERNAME_PASSWORD          0x02
#define SOCKS_HELLO_NO_ACCEPTABLE_METHODS      0xFF

/** Parser de hello con soporte para lecturas parciales */
struct hello_parser {
    /** callback para cada método de autenticación encontrado */
    void (*on_authentication_method)(struct hello_parser *parser, uint8_t method);
    
    /** datos del usuario disponibles en el callback */
    void *data;
    
    /******** campos internos del parser ********/
    enum hello_state state;
    
    /** cantidad de métodos que faltan por leer */
    uint8_t remaining;
};

/** inicializa el parser */
void hello_parser_init(struct hello_parser *p);

/** entrega un byte al parser. retorna el nuevo estado */
enum hello_state hello_parser_feed(struct hello_parser *p, uint8_t b);

/**
 * consume los bytes del buffer hasta que se complete el mensaje 
 * o se produzca un error.
 *
 * @param errored  se setea a true si hay un error de parseo
 * @return  el estado final del parser
 */
enum hello_state hello_consume(buffer *b, struct hello_parser *p, bool *errored);

/**
 * Verifica si el parser está en un estado terminal (done o error)
 */
bool hello_is_done(const enum hello_state state, bool *errored);

/**
 * Arma la respuesta al mensaje hello en el buffer de escritura.
 * 
 * @param b      buffer de escritura
 * @param method método de autenticación seleccionado (0xFF si ninguno)
 * @return  -1 si no hay espacio en el buffer
 */
int hello_marshall(buffer *b, const uint8_t method);

/** libera recursos del parser */
void hello_parser_close(struct hello_parser *p);

/** Descripción textual del estado */
const char *hello_error_description(const struct hello_parser *p);

#endif

