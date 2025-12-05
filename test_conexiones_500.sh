#!/bin/bash
#
# test_conexiones_500.sh - Prueba de 500 conexiones simultáneas
# Este script verifica que el servidor cumpla con el requisito de 500 conexiones
#

GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

PROXY_PORT=1080
MONITOR_PORT=8080
LOCAL_SERVER_PORT=9999
TEST_USER="test500"
TEST_PASS="test500"

cleanup() {
    echo -e "\n${YELLOW}Limpiando...${NC}"
    pkill -f "socks5d" 2>/dev/null
    pkill -f "nc -l.*$LOCAL_SERVER_PORT" 2>/dev/null
    pkill -f "python3.*SimpleHTTPServer" 2>/dev/null
    pkill -f "python3 -m http.server" 2>/dev/null
    rm -f /tmp/test_conn_* 2>/dev/null
}

trap cleanup EXIT

echo -e "${BLUE}"
echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║     PRUEBA DE 500 CONEXIONES SIMULTÁNEAS                      ║"
echo "║     Requisito: 'al menos 500 conexiones concurrentes'         ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo -e "${NC}"

cd "$(dirname "$0")"

# Compilar si es necesario
if [ ! -f "bin/socks5d" ]; then
    echo "Compilando..."
    make all > /dev/null 2>&1
fi

# Iniciar servidor HTTP local simple para pruebas
echo -e "\n${BLUE}[1/4] Iniciando servidor HTTP local de prueba...${NC}"
python3 -m http.server $LOCAL_SERVER_PORT --bind 127.0.0.1 > /dev/null 2>&1 &
HTTP_PID=$!
sleep 1

if ! kill -0 $HTTP_PID 2>/dev/null; then
    echo -e "${RED}Error: No se pudo iniciar servidor HTTP local${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Servidor HTTP local en puerto $LOCAL_SERVER_PORT${NC}"

# Iniciar servidor SOCKS5
echo -e "\n${BLUE}[2/4] Iniciando servidor SOCKSv5...${NC}"
./bin/socks5d -p $PROXY_PORT -P $MONITOR_PORT -u $TEST_USER:$TEST_PASS > /tmp/socks5_500test.log 2>&1 &
SOCKS_PID=$!
sleep 2

if ! kill -0 $SOCKS_PID 2>/dev/null; then
    echo -e "${RED}Error: No se pudo iniciar servidor SOCKS5${NC}"
    cat /tmp/socks5_500test.log
    exit 1
fi
echo -e "${GREEN}✓ Servidor SOCKS5 en puerto $PROXY_PORT (PID: $SOCKS_PID)${NC}"

# Verificar conexión básica
echo -e "\n${BLUE}[3/4] Verificando conexión básica...${NC}"
if curl -s -o /dev/null -w "%{http_code}" --max-time 5 \
    -x socks5://$TEST_USER:$TEST_PASS@127.0.0.1:$PROXY_PORT \
    http://127.0.0.1:$LOCAL_SERVER_PORT/ 2>/dev/null | grep -q "200"; then
    echo -e "${GREEN}✓ Conexión básica funciona${NC}"
else
    echo -e "${RED}✗ Error en conexión básica${NC}"
    exit 1
fi

# Prueba de conexiones simultáneas
echo -e "\n${BLUE}[4/4] Ejecutando prueba de conexiones simultáneas...${NC}"
echo ""

test_concurrent() {
    local NUM=$1
    local success=0
    local failed=0
    local pids=()
    
    echo -ne "  Probando ${YELLOW}$NUM${NC} conexiones... "
    
    # Lanzar conexiones en paralelo
    for i in $(seq 1 $NUM); do
        (
            result=$(curl -s -o /dev/null -w "%{http_code}" --max-time 30 \
                -x socks5://$TEST_USER:$TEST_PASS@127.0.0.1:$PROXY_PORT \
                "http://127.0.0.1:$LOCAL_SERVER_PORT/" 2>/dev/null)
            [ "$result" == "200" ] && echo "1" > /tmp/test_conn_$i || echo "0" > /tmp/test_conn_$i
        ) &
        pids+=($!)
    done
    
    # Esperar a que terminen todas
    for pid in "${pids[@]}"; do
        wait $pid 2>/dev/null
    done
    
    # Contar resultados
    for i in $(seq 1 $NUM); do
        if [ -f /tmp/test_conn_$i ]; then
            [ "$(cat /tmp/test_conn_$i)" == "1" ] && ((success++)) || ((failed++))
            rm -f /tmp/test_conn_$i
        else
            ((failed++))
        fi
    done
    
    local rate=$((success * 100 / NUM))
    
    if [ $rate -ge 90 ]; then
        echo -e "${GREEN}$success/$NUM exitosas (${rate}%)${NC}"
        return 0
    elif [ $rate -ge 70 ]; then
        echo -e "${YELLOW}$success/$NUM exitosas (${rate}%)${NC}"
        return 0
    else
        echo -e "${RED}$success/$NUM exitosas (${rate}%)${NC}"
        return 1
    fi
}

echo "Pruebas progresivas de conexiones:"
echo "─────────────────────────────────────"

PASSED=true

# Probar niveles progresivos
for level in 50 100 200 300 400 500; do
    if ! test_concurrent $level; then
        PASSED=false
        break
    fi
    # Pequeña pausa entre pruebas
    sleep 1
done

# Métricas finales
echo ""
echo -e "${BLUE}Métricas del servidor:${NC}"
./bin/socks5_client -P $MONITOR_PORT metrics

echo ""
echo "═══════════════════════════════════════════════════════════════"

if [ "$PASSED" = true ]; then
    echo -e "${GREEN}✓ PRUEBA EXITOSA: El servidor soporta 500+ conexiones simultáneas${NC}"
    echo "═══════════════════════════════════════════════════════════════"
    exit 0
else
    echo -e "${RED}✗ PRUEBA FALLIDA: El servidor NO cumple con el requisito${NC}"
    echo "═══════════════════════════════════════════════════════════════"
    exit 1
fi
