#ifndef REQUEST_H_nIEvtqIeT9afRVahMPSRock0oPe
#define REQUEST_H_nIEvtqIeT9afRVahMPSRock0oPe

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

#include "buffer.h"

/**
 * Parser del mensaje REQUEST de SOCKS5 (RFC 1928)
 *
 *        +----+-----+-------+------+----------+----------+
 *        |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
 *        +----+-----+-------+------+----------+----------+
 *        | 1  |  1  | X'00' |  1   | Variable |    2     |
 *        +----+-----+-------+------+----------+----------+
 *
 * CMD:
 *   o  CONNECT X'01'
 *   o  BIND X'02'
 *   o  UDP ASSOCIATE X'03'
 *
 * ATYP:
 *   o  IP V4 address: X'01'  (4 bytes)
 *   o  DOMAINNAME: X'03'     (1 byte len + domainname)
 *   o  IP V6 address: X'04'  (16 bytes)
 */

/** Comandos SOCKS5 */
enum socks_cmd {
    socks_req_cmd_connect       = 0x01,
    socks_req_cmd_bind          = 0x02,
    socks_req_cmd_udp_associate = 0x03,
};

/** Tipos de dirección */
enum socks_addr_type {
    socks_req_addrtype_ipv4   = 0x01,
    socks_req_addrtype_domain = 0x03,
    socks_req_addrtype_ipv6   = 0x04,
};

/** Códigos de respuesta SOCKS5 */
enum socks_reply_status {
    socks_status_succeeded                  = 0x00,
    socks_status_general_SOCKS_server_failure = 0x01,
    socks_status_connection_not_allowed     = 0x02,
    socks_status_network_unreachable        = 0x03,
    socks_status_host_unreachable           = 0x04,
    socks_status_connection_refused         = 0x05,
    socks_status_ttl_expired                = 0x06,
    socks_status_command_not_supported      = 0x07,
    socks_status_address_type_not_supported = 0x08,
};

/** Estados del parser de request */
enum request_state {
    request_version,
    request_cmd,
    request_rsv,
    request_atyp,
    request_dstaddr_fqdn_len,
    request_dstaddr,
    request_dstport,
    request_done,
    request_error_unsupported_version,
    request_error_unsupported_atyp,
    request_error_unsupported_cmd,
    request_error,
};

#define SOCKS_MAX_FQDN_LEN 255

/** Estructura para almacenar una dirección SOCKS5 */
union socks_addr {
    struct in_addr  ipv4;
    struct in6_addr ipv6;
    char            fqdn[SOCKS_MAX_FQDN_LEN + 1];
};

/** Datos parseados del request */
struct request {
    enum socks_cmd       cmd;
    enum socks_addr_type dest_addr_type;
    union socks_addr     dest_addr;
    /** en network byte order (big-endian) */
    in_port_t            dest_port;
};

/** Parser de request con soporte para lecturas parciales */
struct request_parser {
    struct request *request;
    
    /******** campos internos del parser ********/
    enum request_state state;
    
    /** bytes que faltan por leer del campo actual */
    uint8_t remaining;
    
    /** índice actual para el address */
    uint8_t addr_idx;
};

/** inicializa el parser */
void request_parser_init(struct request_parser *p);

/** entrega un byte al parser. retorna el nuevo estado */
enum request_state request_parser_feed(struct request_parser *p, uint8_t b);

/**
 * consume los bytes del buffer hasta que se complete el mensaje 
 * o se produzca un error.
 *
 * @param errored  se setea a true si hay un error de parseo
 * @return  el estado final del parser
 */
enum request_state request_consume(buffer *b, struct request_parser *p, bool *errored);

/**
 * Verifica si el parser está en un estado terminal (done o error)
 */
bool request_is_done(const enum request_state state, bool *errored);

/**
 * Arma la respuesta al mensaje request en el buffer de escritura.
 * 
 * @param b       buffer de escritura
 * @param status  código de respuesta SOCKS5
 * @param atyp    tipo de dirección de la respuesta
 * @param addr    dirección (puede ser NULL para 0.0.0.0)
 * @param port    puerto en network byte order
 * @return  -1 si no hay espacio en el buffer
 */
int request_marshall(buffer *b, 
                     const enum socks_reply_status status,
                     const enum socks_addr_type atyp,
                     const union socks_addr *addr,
                     const in_port_t port);

/** libera recursos del parser */
void request_parser_close(struct request_parser *p);

/**
 * Convierte el errno del connect() a un código de respuesta SOCKS5
 */
enum socks_reply_status errno_to_socks(const int e);

/** Descripción textual del estado de error */
const char *request_error_description(const struct request_parser *p);

#endif

