#ifndef MONITORING_H_AbCdEfGhIjKlMnOpQrStUvWxYz
#define MONITORING_H_AbCdEfGhIjKlMnOpQrStUvWxYz

#include "selector.h"

/**
 * Servidor de Monitoreo y Configuración
 *
 * Protocolo binario simple sobre TCP que permite:
 * - Obtener métricas del servidor
 * - Listar usuarios
 * - Agregar/Eliminar usuarios
 *
 * Formato de mensaje:
 * +------+--------+------+----------+
 * | VER  | CMD    | LEN  | DATA     |
 * +------+--------+------+----------+
 * |  1   |   1    |  2   | Variable |
 * +------+--------+------+----------+
 *
 * VER: versión del protocolo (0x01)
 *
 * Comandos:
 *   0x00 - GET_METRICS     - Obtener métricas
 *   0x01 - LIST_USERS      - Listar usuarios
 *   0x02 - ADD_USER        - Agregar usuario (DATA: ulen + user + plen + pass)
 *   0x03 - REMOVE_USER     - Eliminar usuario (DATA: ulen + user)
 *   0x04 - TOGGLE_DISECTOR - Habilitar/deshabilitar disector
 *
 * Respuesta:
 * +------+--------+------+----------+
 * | VER  | STATUS | LEN  | DATA     |
 * +------+--------+------+----------+
 * |  1   |   1    |  2   | Variable |
 * +------+--------+------+----------+
 *
 * STATUS:
 *   0x00 - OK
 *   0x01 - Error general
 *   0x02 - Comando no soportado
 *   0x03 - Usuario no encontrado
 *   0x04 - Usuario ya existe
 *   0x05 - Límite de usuarios alcanzado
 */

#define MONITORING_VERSION 0x01

/** Comandos del protocolo */
enum monitoring_cmd {
    MONITORING_CMD_GET_METRICS     = 0x00,
    MONITORING_CMD_LIST_USERS      = 0x01,
    MONITORING_CMD_ADD_USER        = 0x02,
    MONITORING_CMD_REMOVE_USER     = 0x03,
    MONITORING_CMD_TOGGLE_DISECTOR = 0x04,
};

/** Códigos de respuesta */
enum monitoring_status {
    MONITORING_STATUS_OK                = 0x00,
    MONITORING_STATUS_ERROR             = 0x01,
    MONITORING_STATUS_CMD_NOT_SUPPORTED = 0x02,
    MONITORING_STATUS_USER_NOT_FOUND    = 0x03,
    MONITORING_STATUS_USER_EXISTS       = 0x04,
    MONITORING_STATUS_USER_LIMIT        = 0x05,
};

/**
 * Handler para el socket pasivo que acepta conexiones de monitoreo
 */
void monitoring_passive_accept(struct selector_key *key);

/**
 * Libera recursos del servidor de monitoreo
 */
void monitoring_destroy(void);

#endif

