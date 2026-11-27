// Centraliza parâmetros de segurança, rede e intervalo da OTA.
#pragma once

#include <cstddef>
#include <cstdint>

namespace configuracao {

// Habilitação e origem do manifesto de versões.
constexpr bool HABILITAR_OTA = true;
constexpr char ENDERECO_MANIFESTO_OTA[] =
    "https://github.com/thegm-Gabriel/instrumentacao-ElectricCarUFSM/"
    "releases/latest/download/manifesto-firmwares.json";

// Consulta de versões e janela para autorizar uma atualização disponível.
constexpr uint32_t INTERVALO_VERIFICACAO_OTA_PADRAO_MIN = 15;
constexpr uint32_t INTERVALO_VERIFICACAO_OTA_MINIMO_MIN = 1;
constexpr uint32_t INTERVALO_VERIFICACAO_OTA_MAXIMO_MIN = 7 * 24 * 60;
constexpr uint32_t TEMPO_LIMITE_AUTORIZACAO_OTA_MS = 5 * 60 * 1000;

// Limites do download HTTPS.
constexpr uint32_t TEMPO_LIMITE_HTTP_MS = 20000;
constexpr int TAMANHO_BUFFER_HTTP_TX = 4096;
constexpr int TAMANHO_BUFFER_HTTP_RX_MESTRE = 4096;
constexpr int TEMPO_LEITURA_HTTP_OTA_MS = 2000;
constexpr uint32_t TEMPO_INATIVIDADE_HTTP_OTA_MS = 10000;
constexpr int MAXIMO_REDIRECIONAMENTOS_OTA = 8;
constexpr unsigned MAXIMO_RETOMADAS_OTA_MESTRE = 2;
constexpr uint32_t INTERVALO_RETOMADA_OTA_MS = 500;
constexpr uint32_t TEMPO_RECONEXAO_WIFI_OTA_MS = 8000;
constexpr std::size_t TAMANHO_MAXIMO_MANIFESTO_OTA = 8192;

// Prazo para o controle confirmar uma nova escrita de 0 mV antes da OTA.
constexpr uint32_t TEMPO_CONFIRMACAO_DAC_OTA_MS = 350;

// Verificação de que o satélite voltou com a versão esperada.
constexpr uint32_t TEMPO_LIMITE_CONFIRMACAO_SATELITE_MS = 15000;
constexpr uint32_t INTERVALO_CONFIRMACAO_SATELITE_MS    = 250;

// Transferência de firmware aos satélites pela UART.
constexpr uint32_t TEMPO_RESPOSTA_OTA_UART_MS    = 1500;
constexpr uint32_t TEMPO_INICIO_OTA_UART_MS      = 10000;
constexpr uint32_t TEMPO_FINALIZACAO_OTA_UART_MS = 15000;
constexpr unsigned MAXIMO_TENTATIVAS_OTA_UART = 4;
constexpr unsigned MAXIMO_TENTATIVAS_FINALIZACAO_OTA_UART = 2;
constexpr uint8_t QUANTIDADE_BUFFERS_PIPELINE = 3;

}  // namespace configuracao
