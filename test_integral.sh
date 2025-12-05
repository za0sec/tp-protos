#!/bin/bash

# Colores para output
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== INICIO DE PRUEBA INTEGRAL ===${NC}"

# 0. Limpiar procesos anteriores
pkill -f socks5d 2>/dev/null
sleep 1

# 1. Compilación Limpia
echo -e "\n${BLUE}[1/6] Compilando proyecto...${NC}"
make clean > /dev/null
make all > /dev/null
if [ $? -ne 0 ]; then
    echo -e "${RED}Error de compilación${NC}"
    exit 1
fi
echo -e "${GREEN}Compilación exitosa${NC}"

# 2. Iniciar Servidor
echo -e "\n${BLUE}[2/6] Iniciando Servidor SOCKSv5...${NC}"
# Arrancamos en background (&) y guardamos el PID
./bin/socks5d -p 1080 -P 8080 -u admin:1234 -u juan:1234 -o access.log &
SERVER_PID=$!
echo "Servidor corriendo con PID $SERVER_PID"
sleep 1 # Esperar a que levante

# 3. Pruebas de Gestión (Monitor Client)
echo -e "\n${BLUE}[3/6] Probando Gestión de Usuarios...${NC}"

echo ">> Listar usuarios iniciales (Esperado: admin, juan)"
./bin/socks5_client -P 8080 users

echo -e "\n>> Agregando usuario 'nuevo'..."
./bin/socks5_client -P 8080 -u nuevo:secreto adduser

echo -e "\n>> Intentando duplicar admin (Debe fallar)..."
./bin/socks5_client -P 8080 -u admin:otra adduser

echo -e "\n>> Eliminando usuario 'juan'..."
./bin/socks5_client -P 8080 -u juan deluser

echo -e "\n>> Listar usuarios final (Esperado: admin, nuevo)"
./bin/socks5_client -P 8080 users

echo -e "\n>> Toggle Disector..."
./bin/socks5_client -P 8080 toggle

# 4. Pruebas de Conectividad (Proxy)
echo -e "\n${BLUE}[4/6] Probando Conectividad SOCKSv5...${NC}"

echo -e "\n>> Conexión con 'admin' (Original) - ESPERADO: ÉXITO"
if curl -s -o /dev/null -w "%{http_code}" -x socks5://admin:1234@127.0.0.1:1080 http://www.google.com | grep "200" > /dev/null; then
    echo -e "${GREEN}OK: Conexión exitosa${NC}"
else
    echo -e "${RED}FALLO: No se pudo conectar${NC}"
fi

echo -e "\n>> Conexión con 'nuevo' (Runtime) - ESPERADO: ÉXITO"
if curl -s -o /dev/null -w "%{http_code}" -x socks5://nuevo:secreto@127.0.0.1:1080 http://www.google.com | grep "200" > /dev/null; then
    echo -e "${GREEN}OK: Conexión exitosa${NC}"
else
    echo -e "${RED}FALLO: No se pudo conectar${NC}"
fi

echo -e "\n>> Conexión con 'juan' (Borrado) - ESPERADO: FALLO (403/Rechazado)"
# curl devuelve exit code != 0 si falla el proxy, lo cual es bueno aquí
curl -s -o /dev/null -x socks5://juan:1234@127.0.0.1:1080 http://www.google.com
if [ $? -ne 0 ]; then
    echo -e "${GREEN}OK: Acceso denegado correctamente${NC}"
else
    echo -e "${RED}FALLO: El usuario borrado pudo conectarse${NC}"
fi

echo -e "\n>> Conexión con Password Incorrecto - ESPERADO: FALLO"
curl -s -o /dev/null -x socks5://admin:badpass@127.0.0.1:1080 http://www.google.com
if [ $? -ne 0 ]; then
    echo -e "${GREEN}OK: Acceso denegado correctamente${NC}"
else
    echo -e "${RED}FALLO: Password incorrecto aceptado${NC}"
fi

# 5. Verificación de Métricas
echo -e "\n${BLUE}[5/6] Verificando Métricas Finales...${NC}"
./bin/socks5_client -P 8080 metrics

# 6. Cierre y Logs
echo -e "\n${BLUE}[6/6] Cerrando Servidor y Verificando Logs...${NC}"
kill -SIGINT $SERVER_PID
wait $SERVER_PID 2>/dev/null

echo -e "\n>> Contenido de access.log (Deberían haber 2 entradas):"
if [ -f access.log ]; then
    cat access.log
    rm access.log # Limpieza
else
    echo -e "${RED}Error: No se generó access.log${NC}"
fi

echo -e "\n${BLUE}=== PRUEBA FINALIZADA ===${NC}"
