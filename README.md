# Servidor Proxy SOCKSv5

Implementación de un servidor proxy SOCKSv5 según RFC 1928, con soporte para autenticación usuario/contraseña (RFC 1929).

## Contenido de la entrega

```
.
├── src/                    # Código fuente
├── include/                # Headers
├── doc/
│   ├── informe.pdf        # Informe del proyecto (*)
│   ├── enunciado.txt      # Enunciado del TP
│   └── patches/           # Librerías provistas por la cátedra
├── bin/                   # Ejecutables (generado con make)
├── build/                 # Objetos (generado con make)
├── Makefile
└── README.md
```

(*) El informe debe ser creado aparte.

## Compilación

```bash
make
```

Genera dos ejecutables en `bin/`:
- `socks5d` - Servidor proxy SOCKSv5
- `socks5_client` - Cliente para administración y monitoreo

Para limpiar:
```bash
make clean
```

## Ejecución del servidor

```bash
./bin/socks5d [opciones]
```

### Opciones disponibles

| Opción | Descripción | Valor por defecto |
|--------|-------------|-------------------|
| `-p <puerto>` | Puerto SOCKS5 | 1080 |
| `-l <dirección>` | Dirección de escucha SOCKS5 | 0.0.0.0 |
| `-P <puerto>` | Puerto de administración | 8080 |
| `-L <dirección>` | Dirección de administración | 127.0.0.1 |
| `-u <usuario:clave>` | Usuario del proxy (hasta 10) | ninguno |
| `-o <archivo>` | Archivo de log de accesos | stdout |
| `-N` | Deshabilitar sniffing | habilitado |
| `-v` | Mostrar versión | - |
| `-h` | Mostrar ayuda | - |

### Ejemplos

```bash
# Sin autenticación, puerto por defecto
./bin/socks5d

# Con usuarios y log de accesos
./bin/socks5d -p 1080 -u admin:secreto -u guest:1234 -o /var/log/socks5.log

# Escuchar solo en localhost
./bin/socks5d -l 127.0.0.1 -p 1080
```

## Cliente de administración

```bash
./bin/socks5_client [opciones] <comando>
```

### Opciones

| Opción | Descripción | Valor por defecto |
|--------|-------------|-------------------|
| `-L <dirección>` | Dirección del servidor | 127.0.0.1 |
| `-P <puerto>` | Puerto de administración | 8080 |
| `-u <usuario:clave>` | Credenciales para adduser/deluser | - |

### Comandos disponibles

| Comando | Descripción |
|---------|-------------|
| `metrics` | Muestra estadísticas del servidor |
| `users` | Lista usuarios configurados |
| `adduser` | Agrega usuario (requiere `-u usuario:clave`) |
| `deluser` | Elimina usuario (requiere `-u usuario`) |
| `toggle` | Activa/desactiva sniffing de protocolos |

### Ejemplos

```bash
# Ver métricas
./bin/socks5_client -P 8080 metrics

# Listar usuarios
./bin/socks5_client users

# Agregar usuario en runtime
./bin/socks5_client -u nuevo:clave123 adduser

# Eliminar usuario
./bin/socks5_client -u nuevo deluser
```

## Pruebas del proxy

### Con curl

```bash
# Sin autenticación
curl -x socks5://localhost:1080 http://example.com

# Con autenticación
curl -x socks5://admin:secreto@localhost:1080 http://example.com

# HTTPS a través del proxy
curl -x socks5://admin:secreto@localhost:1080 https://www.google.com
```

### Con Firefox

1. Abrir Preferencias → Configuración de red
2. Seleccionar "Configuración manual del proxy"
3. En "Host SOCKS" poner `localhost`, puerto `1080`
4. Seleccionar "SOCKS v5"
5. Si hay autenticación, Firefox la pedirá al conectar

### Con ssh

```bash
ssh -o ProxyCommand='nc -x localhost:1080 %h %p' usuario@servidor
```

## Protocolo de administración

El servidor de administración escucha en el puerto configurado con `-P` (default 8080) usando un protocolo binario propio.

### Formato de request

```
+------+------+------+----------+
| VER  | CMD  | LEN  | DATA     |
+------+------+------+----------+
|  1   |  1   |  2   | variable |
+------+------+------+----------+
```

- **VER**: Versión del protocolo (0x01)
- **CMD**: Comando
  - 0x00 = Obtener métricas
  - 0x01 = Listar usuarios
  - 0x02 = Agregar usuario
  - 0x03 = Eliminar usuario
  - 0x04 = Toggle sniffing
- **LEN**: Longitud de DATA en bytes (big-endian)
- **DATA**: Datos del comando (depende del CMD)

### Formato de respuesta

```
+------+--------+------+----------+
| VER  | STATUS | LEN  | DATA     |
+------+--------+------+----------+
|  1   |   1    |  2   | variable |
+------+--------+------+----------+
```

- **STATUS**:
  - 0x00 = OK
  - 0x01 = Error general
  - 0x02 = Comando no soportado
  - 0x03 = Error de autenticación
  - 0x04 = Usuario no encontrado
  - 0x05 = Límite de usuarios alcanzado

### Estructura de métricas (CMD 0x00)

La respuesta contiene 6 campos de 8 bytes cada uno (uint64_t big-endian):
1. Conexiones históricas
2. Conexiones concurrentes
3. Bytes totales transferidos
4. Conexiones exitosas
5. Conexiones fallidas
6. Bytes desde clientes

## Registro de accesos

El log de accesos registra cada conexión con el siguiente formato:

```
[YYYY-MM-DD HH:MM:SS] [ACCESS] usuario@cliente:puerto -> destino:puerto ESTADO
```

Ejemplo:
```
[2025-12-04 15:30:45] [ACCESS] admin@192.168.1.100:54321 -> example.com:80 OK
[2025-12-04 15:31:02] [ACCESS] -@192.168.1.101:54322 -> 10.0.0.1:443 FAILED
```

El campo usuario es `-` si no hubo autenticación.

## Métricas disponibles

| Métrica | Descripción |
|---------|-------------|
| Conexiones históricas | Total de conexiones aceptadas desde el inicio |
| Conexiones concurrentes | Conexiones activas en este momento |
| Bytes transferidos | Total de bytes en ambas direcciones |
| Conexiones exitosas | Conexiones que completaron el handshake SOCKS5 |
| Conexiones fallidas | Conexiones que fallaron en algún punto |
| Bytes desde clientes | Bytes recibidos de los clientes |

## Arquitectura

### Componentes principales

- **selector.c**: Multiplexor de I/O no bloqueante (basado en `pselect`)
- **stm.c**: Motor de máquina de estados finitos
- **socks5nio.c**: Implementación del protocolo SOCKSv5
- **monitoring.c**: Servidor de administración
- **logger.c**: Sistema de logging de accesos

### Flujo de una conexión

1. Cliente conecta al puerto SOCKS5
2. **HELLO**: Negociación de método de autenticación
3. **AUTH** (opcional): Autenticación usuario/contraseña
4. **REQUEST**: Cliente solicita conexión a destino
5. **RESOLVING** (si es FQDN): Resolución DNS asíncrona
6. **CONNECTING**: Conexión al servidor destino
7. **COPY**: Túnel bidireccional de datos

### Resolución DNS

La resolución de nombres de dominio se realiza en un thread separado usando `pthread` para no bloquear el selector principal. Cuando la resolución termina, notifica al selector mediante `selector_notify_block`.

Si el dominio resuelve a múltiples direcciones IP y la primera falla, el servidor intenta automáticamente con las siguientes.

## Límites

- Máximo ~1000 conexiones simultáneas (limitado por `FD_SETSIZE`)
- Máximo 10 usuarios configurados
- Buffer de I/O: 4096 bytes por dirección por conexión

## Códigos fuente

| Archivo | Descripción |
|---------|-------------|
| `main.c` | Punto de entrada, inicialización |
| `socks5nio.c` | Máquina de estados SOCKSv5 |
| `hello.c` | Parser de mensaje HELLO |
| `auth.c` | Parser de autenticación RFC 1929 |
| `request.c` | Parser de REQUEST |
| `selector.c` | Multiplexor I/O (provisto por cátedra) |
| `stm.c` | Motor de estados (provisto por cátedra) |
| `buffer.c` | Manejo de buffers (provisto por cátedra) |
| `monitoring.c` | Servidor de administración |
| `monitor_client.c` | Cliente de administración |
| `metrics.c` | Recolección de métricas |
| `logger.c` | Logging de accesos |
| `args.c` | Parseo de argumentos |
| `netutils.c` | Utilidades de red |

## Pruebas

### Prueba integral básica

```bash
./test_integral.sh
```

Esta prueba verifica:
- Compilación del proyecto
- Funcionamiento del servidor SOCKS5
- Gestión de usuarios en runtime
- Conectividad a través del proxy
- Sistema de métricas
- Logs de acceso

### Prueba de 500 conexiones simultáneas

```bash
./test_conexiones_500.sh
```

Verifica que el servidor cumpla con el requisito del enunciado de soportar **al menos 500 conexiones simultáneas**. Utiliza un servidor HTTP local para eliminar variables de red externas.

### Pruebas de stress completas

```bash
./test_stress.sh
```

Las pruebas de stress evalúan:
- Máxima cantidad de conexiones simultáneas (hasta 600)
- Degradación del throughput bajo carga
- Latencia del proxy
- Estabilidad bajo carga sostenida (30 segundos)

Los resultados detallados se guardan en:
- `stress_results.txt` - Resultados en texto plano
- `PRUEBAS_STRESS.md` - Informe completo con análisis

#### Resultados de las pruebas de stress

| Métrica                    | Valor              |
|----------------------------|--------------------|
| Conexiones concurrentes    | **500+ (97%)**     |
| Throughput máximo          | ~2 MB/s agregado   |
| Latencia promedio          | 1 ms               |
| Conexiones/segundo         | ~106               |

## Notas

Las librerías `buffer`, `selector` y `stm` fueron provistas por la cátedra. Los archivos originales están en `doc/patches/`.
