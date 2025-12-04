#ifndef BUFFER_H_VelRDAxzvnuFmwEaR0ftrkIinkT
#define BUFFER_H_VelRDAxzvnuFmwEaR0ftrkIinkT

#include <stdbool.h>
#include <unistd.h>  // size_t, ssize_t
#include <stdint.h>

/**
 * buffer.c - buffer con acceso directo (útil para I/O) que mantiene
 *            mantiene puntero de lectura y de escritura.
 */
typedef struct buffer buffer;
struct buffer {
    uint8_t *data;

    /** límite superior del buffer. inmutable */
    uint8_t *limit;

    /** puntero de lectura */
    uint8_t *read;

    /** puntero de escritura */
    uint8_t *write;
};

/**
 * inicializa el buffer sin utilizar el heap
 */
void
buffer_init(buffer *b, const size_t n, uint8_t *data);

/**
 * Retorna un puntero donde se pueden escribir hasta `*nbytes`.
 * Se debe notificar mediante la función `buffer_write_adv'
 */
uint8_t *
buffer_write_ptr(buffer *b, size_t *nbyte);
void
buffer_write_adv(buffer *b, const ssize_t bytes);

uint8_t *
buffer_read_ptr(buffer *b, size_t *nbyte);
void
buffer_read_adv(buffer *b, const ssize_t bytes);

/**
 * obtiene un byte
 */
uint8_t
buffer_read(buffer *b);

/** escribe un byte */
void
buffer_write(buffer *b, uint8_t c);

/**
 * compacta el buffer
 */
void
buffer_compact(buffer *b);

/**
 * Reinicia todos los punteros
 */
void
buffer_reset(buffer *b);

/** retorna true si hay bytes para leer del buffer */
bool
buffer_can_read(buffer *b);

/** retorna true si se pueden escribir bytes en el buffer */
bool
buffer_can_write(buffer *b);

#endif

