// Intervalos e preferências padrão da interface local.
#pragma once

#include <cstdint>

namespace configuracao {

// Terminal serial
// O intervalo do monitor muda a exibição no console.
constexpr bool TERMINAL_CORES_PADRAO         = true;
constexpr bool TERMINAL_APENAS_AVISOS_PADRAO = false;

constexpr uint32_t INTERVALO_MONITOR_SENSORES_MS = 2000;

// Painel OLED
// Cadência de atualização e tempo de permanência das telas.
constexpr uint32_t INTERVALO_PAINEL_LOCAL_MS       = 500;
constexpr uint32_t INTERVALO_ERRO_PAINEL_MS        = 5000;
constexpr uint32_t INTERVALO_PAGINA_SAUDAVEL_MS    = 6000;
constexpr uint32_t INTERVALO_ALTERNANCIA_ALERTA_MS = 3000;
constexpr uint32_t DURACAO_INICIAL_ESTADO_MS       = 8000;
constexpr uint32_t DURACAO_RESUMO_UPLOAD_MS        = 8000;

constexpr uint32_t INTERVALO_LEMBRETE_OTA_MS   = 30000;
constexpr uint32_t DURACAO_LEMBRETE_OTA_MS     = 6000;
constexpr uint32_t INTERVALO_LEMBRETE_AVISO_MS = 16000;
constexpr uint32_t DURACAO_LEMBRETE_AVISO_MS   = 7000;
constexpr uint32_t INTERVALO_LEMBRETE_FALHA_MS = 14000;
constexpr uint32_t DURACAO_LEMBRETE_FALHA_MS   = 10000;

// LEDs e buzzer
// Define quando reavaliar o estado e repetir avisos sonoros.
constexpr uint32_t PASSO_SINALIZACAO_MS            = 20;
constexpr uint32_t INTERVALO_AVALIACAO_SERVICOS_MS = 500;
constexpr uint32_t DURACAO_AVISO_TEMPORARIO_MS     = 3000;
constexpr uint32_t DURACAO_SINAL_OTA_CONCLUIDO_MS   = 3600;
constexpr uint32_t INTERVALO_ALARME_FALHA_MS       = 45000;
constexpr uint32_t INTERVALO_ALARME_OTA_FALHA_MS   = 120000;
constexpr uint32_t INTERVALO_AVISOS_SONOROS_MS     = 30000;

// Relatórios periódicos
// Intervalos iniciais dos quatro grupos exibidos no console.
constexpr uint32_t INTERVALO_RELATORIO_TELEMETRIA_MS = 1000;
constexpr uint32_t INTERVALO_RELATORIO_UART_MS       = 2000;
constexpr uint32_t INTERVALO_RELATORIO_SISTEMA_MS    = 5000;
constexpr uint32_t INTERVALO_RELATORIO_OTA_MS        = 10000;

}  // namespace configuracao
