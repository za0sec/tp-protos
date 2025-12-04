/**
 * monitor_client.c - Cliente de monitoreo para el servidor SOCKSv5
 *
 * Permite consultar métricas, listar usuarios y administrar la configuración
 * del servidor en tiempo de ejecución.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <stdint.h>

#define MONITORING_VERSION 0x01

/** Comandos del protocolo */
enum {
    CMD_GET_METRICS     = 0x00,
    CMD_LIST_USERS      = 0x01,
    CMD_ADD_USER        = 0x02,
    CMD_REMOVE_USER     = 0x03,
    CMD_TOGGLE_DISECTOR = 0x04,
};

static void
usage(const char *progname) {
    fprintf(stderr,
        "Usage: %s [options] <command>\n"
        "\n"
        "Options:\n"
        "  -h             Show this help\n"
        "  -L <addr>      Server address (default: 127.0.0.1)\n"
        "  -P <port>      Server port (default: 8080)\n"
        "\n"
        "Commands:\n"
        "  metrics        Get server metrics\n"
        "  users          List configured users\n"
        "  adduser        Add user (requires -u user:pass)\n"
        "  deluser        Remove user (requires -u user)\n"
        "  toggle         Toggle disector\n"
        "\n"
        "Examples:\n"
        "  %s metrics\n"
        "  %s -u admin:secret adduser\n"
        "  %s -u admin deluser\n"
        "\n",
        progname, progname, progname, progname);
    exit(1);
}

static int
connect_to_server(const char *addr, unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if(inet_pton(AF_INET, addr, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid address: %s\n", addr);
        close(fd);
        return -1;
    }
    
    if(connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    
    return fd;
}

static int
send_command(int fd, uint8_t cmd, const uint8_t *data, uint16_t data_len) {
    uint8_t header[4];
    header[0] = MONITORING_VERSION;
    header[1] = cmd;
    header[2] = (data_len >> 8) & 0xFF;
    header[3] = data_len & 0xFF;
    
    if(send(fd, header, 4, 0) != 4) {
        perror("send header");
        return -1;
    }
    
    if(data_len > 0 && data != NULL) {
        if(send(fd, data, data_len, 0) != data_len) {
            perror("send data");
            return -1;
        }
    }
    
    return 0;
}

static int
receive_response(int fd, uint8_t *status, uint8_t *data, uint16_t *data_len) {
    uint8_t header[4];
    
    if(recv(fd, header, 4, MSG_WAITALL) != 4) {
        perror("recv header");
        return -1;
    }
    
    if(header[0] != MONITORING_VERSION) {
        fprintf(stderr, "Invalid response version: 0x%02x\n", header[0]);
        return -1;
    }
    
    *status = header[1];
    *data_len = (header[2] << 8) | header[3];
    
    if(*data_len > 0) {
        if(recv(fd, data, *data_len, MSG_WAITALL) != *data_len) {
            perror("recv data");
            return -1;
        }
    }
    
    return 0;
}

static void
cmd_metrics(int fd) {
    if(send_command(fd, CMD_GET_METRICS, NULL, 0) != 0) {
        return;
    }
    
    uint8_t status;
    uint8_t data[1024];
    uint16_t data_len;
    
    if(receive_response(fd, &status, data, &data_len) != 0) {
        return;
    }
    
    if(status != 0) {
        fprintf(stderr, "Error: status = %d\n", status);
        return;
    }
    
    if(data_len >= 48) {
        uint64_t historical = 0, current = 0, bytes = 0;
        uint64_t success = 0, failed = 0, bytes_client = 0;
        
        for(int i = 0; i < 8; i++) {
            historical = (historical << 8) | data[i];
        }
        for(int i = 0; i < 8; i++) {
            current = (current << 8) | data[8 + i];
        }
        for(int i = 0; i < 8; i++) {
            bytes = (bytes << 8) | data[16 + i];
        }
        for(int i = 0; i < 8; i++) {
            success = (success << 8) | data[24 + i];
        }
        for(int i = 0; i < 8; i++) {
            failed = (failed << 8) | data[32 + i];
        }
        for(int i = 0; i < 8; i++) {
            bytes_client = (bytes_client << 8) | data[40 + i];
        }
        
        printf("Server Metrics:\n");
        printf("  Historical connections: %lu\n", historical);
        printf("  Current connections:    %lu\n", current);
        printf("  Total bytes transferred:%lu\n", bytes);
        printf("  Successful connections: %lu\n", success);
        printf("  Failed connections:     %lu\n", failed);
        printf("  Client bytes:           %lu\n", bytes_client);
    }
}

static void
cmd_users(int fd) {
    if(send_command(fd, CMD_LIST_USERS, NULL, 0) != 0) {
        return;
    }
    
    uint8_t status;
    uint8_t data[1024];
    uint16_t data_len;
    
    if(receive_response(fd, &status, data, &data_len) != 0) {
        return;
    }
    
    if(status != 0) {
        fprintf(stderr, "Error: status = %d\n", status);
        return;
    }
    
    if(data_len > 0) {
        uint8_t count = data[0];
        printf("Configured users (%d):\n", count);
        
        size_t offset = 1;
        for(int i = 0; i < count && offset < data_len; i++) {
            uint8_t ulen = data[offset++];
            char username[256];
            memcpy(username, data + offset, ulen);
            username[ulen] = '\0';
            offset += ulen;
            printf("  - %s\n", username);
        }
    }
}

static void
cmd_adduser(int fd, const char *user, const char *pass) {
    uint8_t data[512];
    size_t ulen = strlen(user);
    size_t plen = strlen(pass);
    
    data[0] = ulen;
    memcpy(data + 1, user, ulen);
    data[1 + ulen] = plen;
    memcpy(data + 2 + ulen, pass, plen);
    
    if(send_command(fd, CMD_ADD_USER, data, 2 + ulen + plen) != 0) {
        return;
    }
    
    uint8_t status;
    uint8_t resp[1024];
    uint16_t resp_len;
    
    if(receive_response(fd, &status, resp, &resp_len) != 0) {
        return;
    }
    
    switch(status) {
        case 0x00:
            printf("User '%s' added successfully\n", user);
            break;
        case 0x04:
            fprintf(stderr, "Error: User '%s' already exists\n", user);
            break;
        case 0x05:
            fprintf(stderr, "Error: Maximum users reached\n");
            break;
        default:
            fprintf(stderr, "Error: status = %d\n", status);
            break;
    }
}

static void
cmd_deluser(int fd, const char *user) {
    uint8_t data[256];
    size_t ulen = strlen(user);
    
    data[0] = ulen;
    memcpy(data + 1, user, ulen);
    
    if(send_command(fd, CMD_REMOVE_USER, data, 1 + ulen) != 0) {
        return;
    }
    
    uint8_t status;
    uint8_t resp[1024];
    uint16_t resp_len;
    
    if(receive_response(fd, &status, resp, &resp_len) != 0) {
        return;
    }
    
    switch(status) {
        case 0x00:
            printf("User '%s' removed successfully\n", user);
            break;
        case 0x03:
            fprintf(stderr, "Error: User '%s' not found\n", user);
            break;
        default:
            fprintf(stderr, "Error: status = %d\n", status);
            break;
    }
}

static void
cmd_toggle(int fd) {
    if(send_command(fd, CMD_TOGGLE_DISECTOR, NULL, 0) != 0) {
        return;
    }
    
    uint8_t status;
    uint8_t data[1024];
    uint16_t data_len;
    
    if(receive_response(fd, &status, data, &data_len) != 0) {
        return;
    }
    
    if(status == 0 && data_len > 0) {
        printf("Disector %s\n", data[0] ? "enabled" : "disabled");
    } else {
        fprintf(stderr, "Error: status = %d\n", status);
    }
}

int
main(int argc, char **argv) {
    const char *addr = "127.0.0.1";
    unsigned short port = 8080;
    const char *user_pass = NULL;
    
    int c;
    while((c = getopt(argc, argv, "hL:P:u:")) != -1) {
        switch(c) {
            case 'h':
                usage(argv[0]);
                break;
            case 'L':
                addr = optarg;
                break;
            case 'P':
                port = atoi(optarg);
                break;
            case 'u':
                user_pass = optarg;
                break;
            default:
                usage(argv[0]);
                break;
        }
    }
    
    if(optind >= argc) {
        fprintf(stderr, "Error: missing command\n");
        usage(argv[0]);
    }
    
    const char *cmd = argv[optind];
    
    int fd = connect_to_server(addr, port);
    if(fd < 0) {
        return 1;
    }
    
    if(strcmp(cmd, "metrics") == 0) {
        cmd_metrics(fd);
    } else if(strcmp(cmd, "users") == 0) {
        cmd_users(fd);
    } else if(strcmp(cmd, "adduser") == 0) {
        if(user_pass == NULL) {
            fprintf(stderr, "Error: -u user:pass required\n");
            close(fd);
            return 1;
        }
        char *p = strchr(user_pass, ':');
        if(p == NULL) {
            fprintf(stderr, "Error: format is user:pass\n");
            close(fd);
            return 1;
        }
        *p = '\0';
        cmd_adduser(fd, user_pass, p + 1);
    } else if(strcmp(cmd, "deluser") == 0) {
        if(user_pass == NULL) {
            fprintf(stderr, "Error: -u user required\n");
            close(fd);
            return 1;
        }
        cmd_deluser(fd, user_pass);
    } else if(strcmp(cmd, "toggle") == 0) {
        cmd_toggle(fd);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        close(fd);
        return 1;
    }
    
    close(fd);
    return 0;
}

