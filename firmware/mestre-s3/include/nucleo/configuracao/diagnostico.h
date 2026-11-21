// Cadências de supervisão e limiares de saúde.
#pragma once

#include <cstdint>

namespace configuracao {

// Avaliação e supervisão
// Cadência das duas tarefas e margem de atraso da telemetria.
constexpr uint32_t INTERVALO_DIAGNOSTICO_MS        = 1000;
constexpr uint32_t INTERVALO_SUPERVISAO_MS         = 1000;
constexpr uint32_t MARGEM_SUPERVISAO_TELEMETRIA_MS = 5000;

// RAM interna
// Limiares de memória livre total e do maior bloco contíguo.
constexpr uint32_t HEAP_INTERNO_EMERGENCIA_BYTES  = 8U * 1024U;
constexpr uint32_t BLOCO_INTERNO_EMERGENCIA_BYTES = 2U * 1024U;
constexpr uint32_t HEAP_INTERNO_CRITICO_BYTES     = 16U * 1024U;
constexpr uint32_t BLOCO_INTERNO_CRITICO_BYTES    = 4U * 1024U;
constexpr uint32_t HEAP_INTERNO_ATENCAO_BYTES     = 24U * 1024U;
constexpr uint32_t BLOCO_INTERNO_ATENCAO_BYTES    = 8U * 1024U;

// PSRAM
// Acompanha os mesmos limiares para a memória externa.
constexpr uint32_t PSRAM_EMERGENCIA_BYTES       = 64U * 1024U;
constexpr uint32_t BLOCO_PSRAM_EMERGENCIA_BYTES = 8U * 1024U;
constexpr uint32_t PSRAM_CRITICA_BYTES          = 128U * 1024U;
constexpr uint32_t BLOCO_PSRAM_CRITICO_BYTES    = 16U * 1024U;
constexpr uint32_t PSRAM_ATENCAO_BYTES          = 256U * 1024U;
constexpr uint32_t BLOCO_PSRAM_ATENCAO_BYTES    = 32U * 1024U;

// Confirmação de falta de memória
// Quantidade de avaliações antes de sinalizar ou limpar um alerta.
constexpr uint8_t AMOSTRAS_CRITICAS_MEMORIA    = 3;
constexpr uint8_t AMOSTRAS_ALERTA_MEMORIA      = 15;
constexpr uint8_t AMOSTRAS_RECUPERACAO_MEMORIA = 5;

constexpr uint32_t PILHA_CRITICA_BYTES = 256;

// Comunicação
// Quantidade de avaliações para detectar perda de resposta e Wi-Fi fraco.
constexpr uint8_t AMOSTRAS_SEM_RESPOSTA    = 3;
constexpr uint8_t AMOSTRAS_WIFI_FRACO      = 10;
constexpr uint8_t AMOSTRAS_WIFI_RECUPERADO = 5;

}  // namespace configuracao
