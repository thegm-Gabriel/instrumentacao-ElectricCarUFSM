#pragma once

#include <cstdint>

#include "dados_telemetria_veiculo.h"
#include "esp_err.h"
#include "protocolo_telemetria.h"

struct EstatisticasTelemetria {
    uint16_t ultima_sequencia = 0;
    uint32_t pacotes_gerados = 0;
    uint32_t pacotes_com_falha = 0;
    uint16_t ultimo_crc16 = 0;
};

enum class SateliteTelemetria : uint8_t {
    Equipe,
    Visitantes,
};

struct EstatisticasLinkTelemetria {
    bool respondeu = false;
    uint16_t ultima_sequencia_confirmada = 0;
    uint32_t confirmacoes_recebidas = 0;
    uint32_t confirmacoes_invalidas = 0;
    uint32_t latencia_atual_ms = 0;
    uint32_t latencia_media_ms = 0;
    uint32_t maior_latencia_ms = 0;
    uint32_t idade_ultima_confirmacao_ms = UINT32_MAX;
    uint8_t tipo_satelite = 0;
    uint8_t versao_maior = 0;
    uint8_t versao_menor = 0;
    uint8_t versao_correcao = 0;
    uint32_t capacidades = 0;
};

esp_err_t servico_telemetria_iniciar();
esp_err_t servico_telemetria_enviar(const DadosTelemetriaVeiculo& dados);
EstatisticasTelemetria servico_telemetria_obter_estatisticas();
DadosTelemetriaVeiculo servico_telemetria_obter_ultimos_dados();
EstatisticasLinkTelemetria servico_telemetria_obter_estatisticas_link(
    SateliteTelemetria satelite);
void servico_telemetria_receber_confirmacao(
    SateliteTelemetria satelite, const confirmacao_telemetria_t& confirmacao);

// Durante a OTA dos satélites a telemetria continua ativa neste intervalo.
esp_err_t servico_telemetria_definir_intervalo_durante_ota(uint32_t intervalo_ms);
uint32_t servico_telemetria_obter_intervalo_durante_ota();
uint32_t servico_telemetria_obter_intervalo_atual();
void servico_telemetria_definir_ota_satelite_ativa(bool ativa);
