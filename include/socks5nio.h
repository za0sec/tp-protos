#ifndef SOCKS5NIO_H_FLpPQJoHvLcDkRdTyYsMnUpAeWiZbXgG
#define SOCKS5NIO_H_FLpPQJoHvLcDkRdTyYsMnUpAeWiZbXgG

#include <netdb.h>
#include "selector.h"

/**
 * Handler para el socket pasivo que acepta conexiones SOCKSv5
 */
void socksv5_passive_accept(struct selector_key *key);

/**
 * Libera recursos del pool de conexiones
 */
void socksv5_pool_destroy(void);

/**
 * Obtiene la cantidad de conexiones activas
 */
unsigned socksv5_get_connection_count(void);

#endif

