#ifndef SELECTOR_H_W50GNLODsARolpHbsDsrvYvMsbT
#define SELECTOR_H_W50GNLODsARolpHbsDsrvYvMsbT

#include <sys/time.h>
#include <stdbool.h>

/**
 * selector.c - un muliplexor de entrada salida
 *
 * Un selector permite manejar en un único hilo de ejecución la entrada salida
 * de file descriptors de forma no bloqueante.
 */
typedef struct fdselector * fd_selector;

/** valores de retorno. */
typedef enum {
    /** llamada exitosa */
    SELECTOR_SUCCESS  = 0,
    /** no pudimos alocar memoria */
    SELECTOR_ENOMEM   = 1,
    /** llegamos al límite de descriptores que la plataforma puede manejar */
    SELECTOR_MAXFD    = 2,
    /** argumento ilegal */
    SELECTOR_IARGS    = 3,
    /** descriptor ya está en uso */
    SELECTOR_FDINUSE  = 4,
    /** I/O error check errno */
    SELECTOR_IO       = 5,
} selector_status;

/** retorna una descripción humana del fallo */
const char *
selector_error(const selector_status status);

/** opciones de inicialización del selector */
struct selector_init {
    /** señal a utilizar para notificaciones internas */
    const int signal;

    /** tiempo máximo de bloqueo durante `selector_iteratate' */
    struct timespec select_timeout;
};

/** inicializa la librería */
selector_status
selector_init(const struct selector_init *c);

/** deshace la incialización de la librería */
selector_status
selector_close(void);

/* instancia un nuevo selector. returna NULL si no puede instanciar  */
fd_selector
selector_new(const size_t initial_elements);

/** destruye un selector creado por _new. Tolera NULLs */
void
selector_destroy(fd_selector s);

/**
 * Intereses sobre un file descriptor (quiero leer, quiero escribir, …)
 */
typedef enum {
    OP_NOOP    = 0,
    OP_READ    = 1 << 0,
    OP_WRITE   = 1 << 2,
} fd_interest ;

/**
 * Quita un interés de una lista de intereses
 */
#define INTEREST_OFF(FLAG, MASK)  ( (FLAG) & ~(MASK) )

/**
 * Argumento de todas las funciones callback del handler
 */
struct selector_key {
    /** el selector que dispara el evento */
    fd_selector s;
    /** el file descriptor en cuestión */
    int         fd;
    /** dato provisto por el usuario */
    void *      data;
};

/**
 * Manejador de los diferentes eventos..
 */
typedef struct fd_handler {
  void (*handle_read)      (struct selector_key *key);
  void (*handle_write)     (struct selector_key *key);
  void (*handle_block)     (struct selector_key *key);

  /**
   * llamado cuando se se desregistra el fd
   * Seguramente deba liberar los recusos alocados en data.
   */
  void (*handle_close)     (struct selector_key *key);

} fd_handler;

/**
 * registra en el selector `s' un nuevo file descriptor `fd'.
 */
selector_status
selector_register(fd_selector        s,
                  const int          fd,
                  const fd_handler  *handler,
                  const fd_interest  interest,
                  void *data);

/**
 * desregistra un file descriptor del selector
 */
selector_status
selector_unregister_fd(fd_selector   s,
                       const int     fd);

/** permite cambiar los intereses para un file descriptor */
selector_status
selector_set_interest(fd_selector s, int fd, fd_interest i);

/** permite cambiar los intereses para un file descriptor */
selector_status
selector_set_interest_key(struct selector_key *key, fd_interest i);


/**
 * se bloquea hasta que hay eventos disponible y los despacha.
 * Retorna luego de cada iteración, o al llegar al timeout.
 */
selector_status
selector_select(fd_selector s);

/**
 * Método de utilidad que activa O_NONBLOCK en un fd.
 *
 * retorna -1 ante error, y deja detalles en errno.
 */
int
selector_fd_set_nio(const int fd);

/** notifica que un trabajo bloqueante terminó */
selector_status
selector_notify_block(fd_selector s,
                 const int   fd);

#endif

