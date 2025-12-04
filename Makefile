# =============================================================================
# Makefile - Servidor Proxy SOCKSv5
# ITBA - Protocolos de Comunicación 2025/2
# =============================================================================

# Compilador y flags
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -pedantic
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
CFLAGS += -I$(INC_DIR)
LDFLAGS = -lpthread

# Directorios
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
BIN_DIR = bin

# Ejecutables
SERVER = $(BIN_DIR)/socks5d
CLIENT = $(BIN_DIR)/socks5_client

# Archivos fuente del servidor
SERVER_SRCS = $(SRC_DIR)/main.c \
              $(SRC_DIR)/args.c \
              $(SRC_DIR)/buffer.c \
              $(SRC_DIR)/selector.c \
              $(SRC_DIR)/stm.c \
              $(SRC_DIR)/netutils.c \
              $(SRC_DIR)/hello.c \
              $(SRC_DIR)/auth.c \
              $(SRC_DIR)/request.c \
              $(SRC_DIR)/socks5nio.c \
              $(SRC_DIR)/metrics.c \
              $(SRC_DIR)/monitoring.c \
              $(SRC_DIR)/logger.c

# Archivos fuente del cliente de monitoreo
CLIENT_SRCS = $(SRC_DIR)/monitor_client.c

# Objetos
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Headers
HEADERS = $(wildcard $(INC_DIR)/*.h)

.PHONY: all clean server client

# Target por defecto
all: server client
	@echo ""
	@echo "Build completo."
	@echo "  Servidor: $(SERVER)"
	@echo "  Cliente:  $(CLIENT)"

# Crear directorios
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# Compilar el servidor
server: $(BUILD_DIR) $(BIN_DIR) $(SERVER)

$(SERVER): $(SERVER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Compilar el cliente de monitoreo
client: $(BUILD_DIR) $(BIN_DIR) $(CLIENT)

$(CLIENT): $(CLIENT_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Regla para compilar archivos .c a .o
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Limpiar
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
