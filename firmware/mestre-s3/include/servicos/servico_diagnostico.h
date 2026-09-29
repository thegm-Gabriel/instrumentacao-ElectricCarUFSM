#pragma once

#include <cstdint>

#include "esp_err.h"

enum class EstadoSaudeSistema : uint8_t {
    Saudavel,
    Recuperando,
    Atencao,
    Falha,
};

enum CausaSaudeSistema : uint32_t {
    CausaSaudeNenhuma = 0,
    CausaSaudeHeapCritico = 1u << 0,
    CausaSaudePilhaCritica = 1u << 1,
    CausaSaudeSensor = 1u << 2,
    CausaSaudeI2c = 1u << 3,
    CausaSaudeUart = 1u << 4,
    CausaSaudeSupervisao = 1u << 5,
    CausaSaudeEquipeSemResposta = 1u << 6,
    CausaSaudeVisitantesSemResposta = 1u << 7,
    CausaSaudeWifiFraco = 1u << 8,
    CausaSaudeOtaRecente = 1u << 9,
};

struct ResumoSaudeSistema {
    EstadoSaudeSistema estado = EstadoSaudeSistema::Saudavel;
    uint64_t tempo_ativo_ms = 0;
    uint32_t heap_livre = 0;
    uint32_t menor_heap_livre = 0;
    uint32_t palavras_pilha_livres = 0;
    uint32_t motivo_ultimo_reset = 0;
    uint32_t erros_sensores = 0;
    uint32_t erros_i2c = 0;
    uint32_t erros_uart = 0;
    uint32_t descartes_uart = 0;
    uint32_t falhas_ota = 0;
    bool wifi_ativo = false;
    bool wifi_conectado = false;
    int8_t wifi_rssi_dbm = 0;
    bool equipe_respondendo = false;
    bool visitantes_respondendo = false;
    uint32_t latencia_equipe_ms = 0;
    uint32_t latencia_visitantes_ms = 0;
    uint32_t falhas_supervisao = 0;
    uint32_t causas_ativas = CausaSaudeNenhuma;
};

struct EstatisticasSensoresDiagnostico {
    uint32_t erros_adc = 0;
    uint32_t erros_mpu6050 = 0;
    esp_err_t ultimo_erro_adc = ESP_OK;
    esp_err_t ultimo_erro_mpu6050 = ESP_OK;
};

esp_err_t servico_diagnostico_iniciar();
void servico_diagnostico_registrar_erro_adc(esp_err_t erro);
void servico_diagnostico_registrar_erro_mpu6050(esp_err_t erro);
EstatisticasSensoresDiagnostico servico_diagnostico_obter_sensores();
ResumoSaudeSistema servico_diagnostico_obter_resumo();
const char* servico_diagnostico_nome_estado(EstadoSaudeSistema estado);
