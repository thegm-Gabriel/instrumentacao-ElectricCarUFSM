#pragma once

#include <cstdint>

#include "dispositivos/mpu6050.h"
#include "esp_err.h"

struct EstatisticasTelemetria {
    uint16_t ultima_sequencia = 0;
    uint32_t pacotes_gerados = 0;
    uint32_t envios_equipe_ok = 0;
    uint32_t envios_visitantes_ok = 0;
    uint32_t erros_equipe = 0;
    uint32_t erros_visitantes = 0;
    uint8_t ultimo_checksum = 0;
};

esp_err_t servico_telemetria_iniciar();
esp_err_t servico_telemetria_enviar(uint16_t tensao_bruta,
                                    const LeituraMpu6050& leitura_mpu);
EstatisticasTelemetria servico_telemetria_obter_estatisticas();
