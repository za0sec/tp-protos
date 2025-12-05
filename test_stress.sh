#!/bin/bash
#
# test_stress.sh - Pruebas de stress para servidor SOCKSv5
# ITBA - Protocolos de Comunicación 2025/2
#
# Este script ejecuta pruebas de stress para evaluar:
# 1. Máxima cantidad de conexiones simultáneas
# 2. Degradación del throughput bajo carga
# 3. Rendimiento general del servidor
#

# Colores para output
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Configuración
PROXY_HOST="127.0.0.1"
PROXY_PORT="1080"
MONITOR_PORT="8080"
TEST_USER="stress"
TEST_PASS="stress123"
RESULTS_FILE="stress_results.txt"
LOG_FILE="stress_test.log"

# Servidor local de pruebas
LOCAL_SERVER_PORT="9999"
LOCAL_SERVER_PID=""

# URLs de prueba - usamos servidor local para evitar problemas de red
TEST_URLS=(
    "http://127.0.0.1:9999/"
)

# Archivo para descargar (para pruebas de throughput)
LARGE_FILE_URL="http://speedtest.tele2.net/1MB.zip"

print_header() {
    echo -e "\n${BLUE}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════════════════${NC}\n"
}

print_result() {
    if [ "$2" == "OK" ]; then
        echo -e "  ${GREEN}✓${NC} $1"
    elif [ "$2" == "WARN" ]; then
        echo -e "  ${YELLOW}⚠${NC} $1"
    else
        echo -e "  ${RED}✗${NC} $1"
    fi
}

cleanup() {
    echo -e "\n${YELLOW}Limpiando...${NC}"
    pkill -f "socks5d" 2>/dev/null
    [ -n "$LOCAL_SERVER_PID" ] && kill $LOCAL_SERVER_PID 2>/dev/null
    pkill -f "python3 -m http.server $LOCAL_SERVER_PORT" 2>/dev/null
    rm -f /tmp/stress_test_* 2>/dev/null
    rm -f /tmp/stress_bytes_* 2>/dev/null
    rm -f /tmp/stress_sustained_* 2>/dev/null
    rm -f "$LOG_FILE" 2>/dev/null
}

trap cleanup EXIT

# Iniciar servidor HTTP local para pruebas
start_local_server() {
    print_header "Iniciando Servidor HTTP Local"
    # Limpiar instancias anteriores
    pkill -f "python3 -m http.server $LOCAL_SERVER_PORT" 2>/dev/null
    sleep 1
    python3 -m http.server $LOCAL_SERVER_PORT --bind 127.0.0.1 > /dev/null 2>&1 &
    LOCAL_SERVER_PID=$!
    sleep 1
    if kill -0 $LOCAL_SERVER_PID 2>/dev/null; then
        print_result "Servidor HTTP local en puerto $LOCAL_SERVER_PORT" "OK"
        return 0
    else
        print_result "Error al iniciar servidor local" "FAIL"
        return 1
    fi
}

# Verificar dependencias
check_dependencies() {
    print_header "Verificando Dependencias"
    
    local missing=0
    
    # Dependencias obligatorias
    for cmd in curl nc timeout; do
        if command -v $cmd &> /dev/null; then
            print_result "$cmd encontrado" "OK"
        else
            print_result "$cmd no encontrado" "FAIL"
            missing=1
        fi
    done
    
    # ab es opcional pero útil
    if command -v ab &> /dev/null; then
        print_result "ab encontrado (opcional)" "OK"
    else
        echo -e "  ${YELLOW}Nota: 'ab' (Apache Benchmark) no está instalado (opcional)."
        echo -e "  Instalar con: sudo apt-get install apache2-utils${NC}"
    fi
    
    return $missing
}

# Iniciar servidor
start_server() {
    print_header "Iniciando Servidor SOCKSv5"
    
    # Limpiar instancias anteriores
    pkill -f "socks5d" 2>/dev/null
    sleep 1
    
    cd "$(dirname "$0")"
    
    # Compilar si es necesario
    if [ ! -f "bin/socks5d" ]; then
        echo "Compilando..."
        make all > /dev/null 2>&1
    fi
    
    # Iniciar servidor
    ./bin/socks5d -p $PROXY_PORT -P $MONITOR_PORT -u $TEST_USER:$TEST_PASS > "$LOG_FILE" 2>&1 &
    SERVER_PID=$!
    sleep 2
    
    if kill -0 $SERVER_PID 2>/dev/null; then
        print_result "Servidor iniciado (PID: $SERVER_PID)" "OK"
        return 0
    else
        print_result "Error al iniciar servidor" "FAIL"
        return 1
    fi
}

# Test 1: Conexiones secuenciales básicas
test_sequential_connections() {
    print_header "Test 1: Conexiones Secuenciales"
    
    local success=0
    local failed=0
    local total=20
    
    echo "Ejecutando $total conexiones secuenciales..."
    
    for i in $(seq 1 $total); do
        if curl -s -o /dev/null -w "%{http_code}" --max-time 5 \
            -x socks5://$TEST_USER:$TEST_PASS@$PROXY_HOST:$PROXY_PORT \
            "${TEST_URLS[0]}" 2>/dev/null | grep -q "200"; then
            ((success++))
        else
            ((failed++))
        fi
    done
    
    print_result "Exitosas: $success / $total" "OK"
    [ $failed -gt 0 ] && print_result "Fallidas: $failed" "WARN"
    
    echo "$total conexiones secuenciales: $success exitosas, $failed fallidas" >> "$RESULTS_FILE"
}

# Test 2: Conexiones concurrentes
test_concurrent_connections() {
    print_header "Test 2: Conexiones Concurrentes"
    
    local levels=(50 100 200 300 400 500 600)
    
    echo "PRUEBA DE CONEXIONES CONCURRENTES" >> "$RESULTS_FILE"
    echo "=================================" >> "$RESULTS_FILE"
    
    for concurrent in "${levels[@]}"; do
        echo -e "\nProbando con ${YELLOW}$concurrent${NC} conexiones concurrentes..."
        
        local success=0
        local failed=0
        local pids=()
        
        # Lanzar conexiones en paralelo
        for i in $(seq 1 $concurrent); do
            (
                result=$(curl -s -o /dev/null -w "%{http_code}" --max-time 30 \
                    -x socks5://$TEST_USER:$TEST_PASS@$PROXY_HOST:$PROXY_PORT \
                    "http://127.0.0.1:$LOCAL_SERVER_PORT/" 2>/dev/null)
                if [ "$result" == "200" ]; then
                    echo "OK" > /tmp/stress_test_$i
                else
                    echo "FAIL" > /tmp/stress_test_$i
                fi
            ) &
            pids+=($!)
        done
        
        # Esperar a que terminen todas
        for pid in "${pids[@]}"; do
            wait $pid 2>/dev/null
        done
        
        # Contar resultados
        for i in $(seq 1 $concurrent); do
            if [ -f /tmp/stress_test_$i ] && grep -q "OK" /tmp/stress_test_$i 2>/dev/null; then
                ((success++))
            else
                ((failed++))
            fi
            rm -f /tmp/stress_test_$i
        done
        
        local success_rate=$((success * 100 / concurrent))
        
        if [ $success_rate -ge 90 ]; then
            print_result "$concurrent concurrentes: $success/$concurrent exitosas (${success_rate}%)" "OK"
        elif [ $success_rate -ge 70 ]; then
            print_result "$concurrent concurrentes: $success/$concurrent exitosas (${success_rate}%)" "WARN"
        else
            print_result "$concurrent concurrentes: $success/$concurrent exitosas (${success_rate}%)" "FAIL"
        fi
        
        echo "$concurrent concurrentes: $success exitosas, $failed fallidas (${success_rate}%)" >> "$RESULTS_FILE"
        
        # Obtener métricas del servidor
        ./bin/socks5_client -P $MONITOR_PORT metrics 2>/dev/null | grep -E "(Historical|Current)" >> "$RESULTS_FILE"
        
        # Si el éxito es muy bajo, detenemos
        if [ $success_rate -lt 50 ]; then
            echo -e "${YELLOW}Deteniendo prueba: tasa de éxito muy baja${NC}"
            break
        fi
        
        sleep 2  # Dar tiempo al servidor para recuperarse
    done
    
    echo "" >> "$RESULTS_FILE"
}

# Test 3: Throughput
test_throughput() {
    print_header "Test 3: Medición de Throughput"
    
    echo "PRUEBA DE THROUGHPUT" >> "$RESULTS_FILE"
    echo "====================" >> "$RESULTS_FILE"
    
    # Test con diferentes tamaños de transferencia
    local sizes=("100KB" "1MB")
    local urls=(
        "http://speedtest.tele2.net/100KB.zip"
        "http://speedtest.tele2.net/1MB.zip"
    )
    
    for i in "${!sizes[@]}"; do
        echo -e "\nDescargando archivo de ${YELLOW}${sizes[$i]}${NC}..."
        
        local start_time=$(date +%s.%N)
        local output=$(curl -s -o /dev/null -w "%{speed_download}" --max-time 60 \
            -x socks5://$TEST_USER:$TEST_PASS@$PROXY_HOST:$PROXY_PORT \
            "${urls[$i]}" 2>/dev/null)
        local end_time=$(date +%s.%N)
        
        if [ -n "$output" ] && [ "$output" != "0.000" ]; then
            # Convertir a KB/s (curl reporta en bytes/s)
            local speed_kbs=$(echo "$output / 1024" | bc -l 2>/dev/null | cut -d'.' -f1)
            if [ -z "$speed_kbs" ] || [ "$speed_kbs" == "0" ]; then
                speed_kbs=$(echo "$output" | cut -d'.' -f1)
                speed_kbs=$((speed_kbs / 1024))
            fi
            print_result "${sizes[$i]}: ~${speed_kbs} KB/s" "OK"
            echo "${sizes[$i]}: ${speed_kbs} KB/s" >> "$RESULTS_FILE"
        else
            print_result "${sizes[$i]}: Falló la descarga" "FAIL"
            echo "${sizes[$i]}: FAILED" >> "$RESULTS_FILE"
        fi
    done
    
    echo "" >> "$RESULTS_FILE"
}

# Test 4: Throughput bajo carga
test_throughput_under_load() {
    print_header "Test 4: Throughput Bajo Carga"
    
    echo "THROUGHPUT BAJO CARGA" >> "$RESULTS_FILE"
    echo "=====================" >> "$RESULTS_FILE"
    
    local concurrent_levels=(1 5 10 20)
    
    for concurrent in "${concurrent_levels[@]}"; do
        echo -e "\nMidiendo throughput con ${YELLOW}$concurrent${NC} descargas paralelas..."
        
        local pids=()
        local total_bytes=0
        local start_time=$(date +%s)
        
        # Lanzar descargas en paralelo
        for i in $(seq 1 $concurrent); do
            (
                bytes=$(curl -s -o /dev/null -w "%{size_download}" --max-time 30 \
                    -x socks5://$TEST_USER:$TEST_PASS@$PROXY_HOST:$PROXY_PORT \
                    "http://speedtest.tele2.net/100KB.zip" 2>/dev/null)
                echo "$bytes" > /tmp/stress_bytes_$i
            ) &
            pids+=($!)
        done
        
        # Esperar
        for pid in "${pids[@]}"; do
            wait $pid 2>/dev/null
        done
        
        local end_time=$(date +%s)
        local duration=$((end_time - start_time))
        [ $duration -eq 0 ] && duration=1
        
        # Sumar bytes
        for i in $(seq 1 $concurrent); do
            if [ -f /tmp/stress_bytes_$i ]; then
                bytes=$(cat /tmp/stress_bytes_$i)
                total_bytes=$((total_bytes + bytes))
                rm -f /tmp/stress_bytes_$i
            fi
        done
        
        local throughput=$((total_bytes / duration / 1024))
        print_result "$concurrent paralelas: ~${throughput} KB/s total, ${duration}s" "OK"
        echo "$concurrent paralelas: ${throughput} KB/s total en ${duration}s" >> "$RESULTS_FILE"
    done
    
    echo "" >> "$RESULTS_FILE"
}

# Test 5: Latencia
test_latency() {
    print_header "Test 5: Medición de Latencia"
    
    echo "PRUEBA DE LATENCIA" >> "$RESULTS_FILE"
    echo "==================" >> "$RESULTS_FILE"
    
    local total_time=0
    local count=10
    
    echo "Midiendo latencia promedio ($count requests)..."
    
    for i in $(seq 1 $count); do
        time_ms=$(curl -s -o /dev/null -w "%{time_total}" --max-time 10 \
            -x socks5://$TEST_USER:$TEST_PASS@$PROXY_HOST:$PROXY_PORT \
            "http://127.0.0.1:$LOCAL_SERVER_PORT/" 2>/dev/null)
        
        # Convertir a milisegundos
        time_ms_int=$(echo "$time_ms * 1000" | bc 2>/dev/null | cut -d'.' -f1)
        [ -z "$time_ms_int" ] && time_ms_int=0
        total_time=$((total_time + time_ms_int))
    done
    
    local avg=$((total_time / count))
    print_result "Latencia promedio: ${avg}ms" "OK"
    echo "Latencia promedio: ${avg}ms" >> "$RESULTS_FILE"
    echo "" >> "$RESULTS_FILE"
}

# Test 6: Estabilidad bajo carga sostenida
test_sustained_load() {
    print_header "Test 6: Carga Sostenida (30 segundos)"
    
    echo "CARGA SOSTENIDA" >> "$RESULTS_FILE"
    echo "===============" >> "$RESULTS_FILE"
    
    local duration=30
    local concurrent=20
    local success=0
    local failed=0
    local start_time=$(date +%s)
    local end_time=$((start_time + duration))
    
    echo "Ejecutando $concurrent conexiones concurrentes durante $duration segundos..."
    
    while [ $(date +%s) -lt $end_time ]; do
        # Lanzar batch de conexiones
        local pids=()
        for i in $(seq 1 $concurrent); do
            (
                result=$(curl -s -o /dev/null -w "%{http_code}" --max-time 10 \
                    -x socks5://$TEST_USER:$TEST_PASS@$PROXY_HOST:$PROXY_PORT \
                    "http://127.0.0.1:$LOCAL_SERVER_PORT/" 2>/dev/null)
                [ "$result" == "200" ] && echo "1" > /tmp/stress_sustained_$i || echo "0" > /tmp/stress_sustained_$i
            ) &
            pids+=($!)
        done
        
        # Esperar a que terminen
        for pid in "${pids[@]}"; do
            wait $pid 2>/dev/null
        done
        
        # Contar resultados de los archivos
        for i in $(seq 1 $concurrent); do
            if [ -f /tmp/stress_sustained_$i ]; then
                [ "$(cat /tmp/stress_sustained_$i 2>/dev/null)" == "1" ] && ((success++)) || ((failed++))
                rm -f /tmp/stress_sustained_$i
            else
                ((failed++))
            fi
        done
        
        # Pequeña pausa entre batches
        sleep 0.5
    done
    
    local total=$((success + failed))
    local rate=0
    [ $total -gt 0 ] && rate=$((success * 100 / total))
    
    print_result "Total: $total conexiones en $duration segundos" "OK"
    print_result "Exitosas: $success (${rate}%)" "OK"
    [ $failed -gt 0 ] && print_result "Fallidas: $failed" "WARN"
    
    echo "Carga sostenida ${duration}s: $success exitosas de $total (${rate}%)" >> "$RESULTS_FILE"
    
    # Métricas finales
    echo -e "\nMétricas del servidor después de carga sostenida:"
    ./bin/socks5_client -P $MONITOR_PORT metrics
    ./bin/socks5_client -P $MONITOR_PORT metrics >> "$RESULTS_FILE"
    echo "" >> "$RESULTS_FILE"
}

# Resumen final
generate_summary() {
    print_header "Resumen de Pruebas de Stress"
    
    echo "═══════════════════════════════════════════════════════════════" >> "$RESULTS_FILE"
    echo "RESUMEN" >> "$RESULTS_FILE"
    echo "═══════════════════════════════════════════════════════════════" >> "$RESULTS_FILE"
    
    # Métricas finales del servidor
    ./bin/socks5_client -P $MONITOR_PORT metrics | tee -a "$RESULTS_FILE"
    
    echo -e "\n${GREEN}Resultados guardados en: $RESULTS_FILE${NC}"
}

# Main
main() {
    echo -e "${BLUE}"
    echo "╔═══════════════════════════════════════════════════════════════╗"
    echo "║         PRUEBAS DE STRESS - Servidor SOCKSv5                  ║"
    echo "║         ITBA - Protocolos de Comunicación 2025/2              ║"
    echo "╚═══════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
    
    # Inicializar archivo de resultados
    echo "RESULTADOS DE PRUEBAS DE STRESS" > "$RESULTS_FILE"
    echo "Fecha: $(date)" >> "$RESULTS_FILE"
    echo "Sistema: $(uname -a)" >> "$RESULTS_FILE"
    echo "" >> "$RESULTS_FILE"
    
    # Verificar dependencias
    check_dependencies || exit 1
    
    # Iniciar servidor HTTP local para pruebas
    start_local_server || exit 1
    
    # Iniciar servidor SOCKS5
    start_server || exit 1
    
    # Ejecutar pruebas
    test_sequential_connections
    test_concurrent_connections
    test_throughput
    test_throughput_under_load
    test_latency
    test_sustained_load
    
    # Generar resumen
    generate_summary
    
    echo -e "\n${GREEN}Pruebas completadas.${NC}"
}

# Ejecutar si se llama directamente
if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
