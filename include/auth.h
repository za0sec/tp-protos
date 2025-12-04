#ifndef AUTH_H_suncraCSodolAsjfasfiABSDABJBDAB
#define AUTH_H_suncraCSodolAsjfasfiABSDABJBDAB

#include <stdint.h>
#include <stdbool.h>

#include "buffer.h"

/**
 * Parser de autenticación Usuario/Contraseña RFC 1929
 *
 * Una vez negociada la autenticación con username/password, 
 * el cliente envía:
 *
 *           +----+------+----------+------+----------+
 *           |VER | ULEN |  UNAME   | PLEN |  PASSWD  |
 *           +----+------+----------+------+----------+
 *           | 1  |  1   | 1 to 255 |  1   | 1 to 255 |
 *           +----+------+----------+------+----------+
 *
 * VER: versión del subnegociado (0x01)
 */

#define AUTH_VERSION 0x01

#define AUTH_MAX_USERNAME_LEN 255
#define AUTH_MAX_PASSWORD_LEN 255

/** Estados del parser de autenticación */
enum auth_state {
    auth_version,
    auth_ulen,
    auth_uname,
    auth_plen,
    auth_passwd,
    auth_done,
    auth_error_unsupported_version,
    auth_error,
};

/** Parser de autenticación con soporte para lecturas parciales */
struct auth_parser {
    /******** campos internos del parser ********/
    enum auth_state state;
    
    /** bytes que faltan por leer del campo actual */
    uint8_t remaining;
    
    /** índice actual en el username/password */
    uint8_t idx;
    
    /** username parseado */
    uint8_t username[AUTH_MAX_USERNAME_LEN + 1];
    uint8_t username_len;
    
    /** password parseado */
    uint8_t password[AUTH_MAX_PASSWORD_LEN + 1];
    uint8_t password_len;
};

/** inicializa el parser */
void auth_parser_init(struct auth_parser *p);

/** entrega un byte al parser. retorna el nuevo estado */
enum auth_state auth_parser_feed(struct auth_parser *p, uint8_t b);

/**
 * consume los bytes del buffer hasta que se complete el mensaje 
 * o se produzca un error.
 *
 * @param errored  se setea a true si hay un error de parseo
 * @return  el estado final del parser
 */
enum auth_state auth_consume(buffer *b, struct auth_parser *p, bool *errored);

/**
 * Verifica si el parser está en un estado terminal (done o error)
 */
bool auth_is_done(const enum auth_state state, bool *errored);

/**
 * Arma la respuesta al mensaje de autenticación.
 * 
 * @param b       buffer de escritura
 * @param status  0x00 = success, cualquier otro = failure
 * @return  -1 si no hay espacio en el buffer
 */
int auth_marshall(buffer *b, const uint8_t status);

/** libera recursos del parser */
void auth_parser_close(struct auth_parser *p);

#endif

