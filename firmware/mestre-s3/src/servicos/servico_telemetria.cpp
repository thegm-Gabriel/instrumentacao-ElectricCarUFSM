#include "servicos/servico_telemetria.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>

#include "gerenciadores/gerenciador_uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nucleo/configuracao_placa.h"
#include "protocolo_telemetria.h"

namespace {
constexpr char ETIQUETA[] = "servico_telemetria";
static_assert(TAMANHO_PACOTE_TELEMETRIA <= configuracao::TAMANHO_MAXIMO_MENSAGEM_UART,
              "O quadro de telemetria excede o limite da fila UART");

uint16_t proxima_sequencia = 0;
EstatisticasTelemetria estatisticas{};
DadosTelemetriaVeiculo ultimos_dados{};
portMUX_TYPE trava_estatisticas = portMUX_INITIALIZER_UNLOCKED;
uint32_t intervalo_durante_ota_ms =
    configuracao::INTERVALO_TELEMETRIA_OTA_PADRAO_MS;
bool ota_satelite_ativa = false;
bool iniciado = false;

struct MonitorConfirmacao {
    SateliteTelemetria satelite;
    const char* nome;
    uint32_t instante_ultima_confirmacao_ms;
    uint32_t ultimo_log_identidade_ms;
    EstatisticasLinkTelemetria estatisticas;
};

MonitorConfirmacao monitores[] = {
    {SateliteTelemetria::Equipe, "equipe", 0, 0, {}},
    {SateliteTelemetria::Visitantes, "visitantes", 0, 0, {}},
};

MonitorConfirmacao* encontrar_monitor(SateliteTelemetria satelite) {
    for (auto& monitor : monitores) {
        if (monitor.satelite == satelite) return &monitor;
    }
    return nullptr;
}

void registrar_confirmacao(MonitorConfirmacao& monitor,
                           const confirmacao_telemetria_t& confirmacao) {
    const uint8_t tipo_esperado = monitor.satelite == SateliteTelemetria::Equipe
                                      ? SATELITE_TELEMETRIA_EQUIPE
                                      : SATELITE_TELEMETRIA_VISITANTES;
    if (confirmacao.tipo_satelite != tipo_esperado) {
        const uint32_t agora_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        portENTER_CRITICAL(&trava_estatisticas);
        monitor.estatisticas.confirmacoes_invalidas++;
        const bool deve_registrar = monitor.ultimo_log_identidade_ms == 0 ||
                                     agora_ms - monitor.ultimo_log_identidade_ms >= 5000u;
        if (deve_registrar) monitor.ultimo_log_identidade_ms = agora_ms;
        portEXIT_CRITICAL(&trava_estatisticas);
        if (deve_registrar) {
            ESP_LOGE(ETIQUETA,
                     "%s respondeu como tipo %u; esperado=%u. Verifique a ligação das UARTs",
                     monitor.nome, static_cast<unsigned>(confirmacao.tipo_satelite),
                     static_cast<unsigned>(tipo_esperado));
        }
        return;
    }
    const uint32_t agora_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint32_t latencia_ms = agora_ms - confirmacao.tempo_mestre_ms;
    portENTER_CRITICAL(&trava_estatisticas);
    auto& link = monitor.estatisticas;
    link.respondeu = true;
    link.ultima_sequencia_confirmada = confirmacao.sequencia;
    link.tipo_satelite = confirmacao.tipo_satelite;
    link.versao_maior = confirmacao.versao_maior;
    link.versao_menor = confirmacao.versao_menor;
    link.versao_correcao = confirmacao.versao_correcao;
    link.capacidades = confirmacao.capacidades;
    link.latencia_atual_ms = latencia_ms;
    link.latencia_media_ms = link.confirmacoes_recebidas == 0
                                 ? latencia_ms
                                 : (link.latencia_media_ms * 7u + latencia_ms) / 8u;
    if (latencia_ms > link.maior_latencia_ms) link.maior_latencia_ms = latencia_ms;
    link.confirmacoes_recebidas++;
    monitor.instante_ultima_confirmacao_ms = agora_ms;
    portEXIT_CRITICAL(&trava_estatisticas);
}

uint16_t escalar_uint16(float valor, float fator) {
    return static_cast<uint16_t>(std::lround(
        std::clamp(valor * fator, 0.0f, 65535.0f)));
}

int16_t escalar_int16(float valor, float fator) {
    return static_cast<int16_t>(std::lround(
        std::clamp(valor * fator, -32768.0f, 32767.0f)));
}

uint32_t escalar_uint32(float valor, float fator) {
    const double escalado = static_cast<double>(valor) * fator;
    return static_cast<uint32_t>(std::llround(
        std::clamp(escalado, 0.0, 4294967295.0)));
}

bool dados_validos(const DadosTelemetriaVeiculo& dados) {
    if (dados.tensao_adc_bruta > 4095 || static_cast<uint8_t>(dados.marcha) > 3 ||
        !std::isfinite(dados.velocidade_kmh) || !std::isfinite(dados.aceleracao_ms2) ||
        !std::isfinite(dados.acelerador_percentual) ||
        !std::isfinite(dados.freio_percentual) ||
        !std::isfinite(dados.tensao_pacote_v) || !std::isfinite(dados.corrente_a) ||
        !std::isfinite(dados.carga_percentual) || !std::isfinite(dados.latitude) ||
        !std::isfinite(dados.longitude) || !std::isfinite(dados.rumo_graus) ||
        !std::isfinite(dados.potencia_w) || !std::isfinite(dados.autonomia_km) ||
        !std::isfinite(dados.percurso_km) || !std::isfinite(dados.odometro_km) ||
        dados.acelerador_percentual < 0.0f || dados.acelerador_percentual > 100.0f ||
        dados.freio_percentual < 0.0f || dados.freio_percentual > 100.0f ||
        dados.carga_percentual < 0.0f || dados.carga_percentual > 100.0f ||
        dados.latitude < -90.0 || dados.latitude > 90.0 ||
        dados.longitude < -180.0 || dados.longitude > 180.0) {
        return false;
    }
    for (float tensao : dados.tensoes_celulas_v) {
        if (!std::isfinite(tensao) || tensao < 0.0f) return false;
    }
    for (float distancia : dados.distancias_cm) {
        if (!std::isfinite(distancia) || distancia < 0.0f) return false;
    }
    return true;
}

}  // namespace

esp_err_t servico_telemetria_iniciar() {
    if (iniciado) return ESP_OK;
    iniciado = true;
    ESP_LOGI(ETIQUETA,
             "Serviço pronto para publicar dados e acompanhar confirmações");
    return ESP_OK;
}

void servico_telemetria_receber_confirmacao(
    SateliteTelemetria satelite, const confirmacao_telemetria_t& confirmacao) {
    MonitorConfirmacao* monitor = encontrar_monitor(satelite);
    if (monitor == nullptr) return;
    if (protocolo_telemetria_confirmacao_valida(&confirmacao)) {
        registrar_confirmacao(*monitor, confirmacao);
    } else {
        portENTER_CRITICAL(&trava_estatisticas);
        monitor->estatisticas.confirmacoes_invalidas++;
        portEXIT_CRITICAL(&trava_estatisticas);
    }
}

esp_err_t servico_telemetria_enviar(const DadosTelemetriaVeiculo& dados) {
    if (!iniciado) return ESP_ERR_INVALID_STATE;
    if (!dados_validos(dados)) {
        ESP_LOGE(ETIQUETA, "Pacote rejeitado: dados ausentes, não finitos ou fora da faixa");
        return ESP_ERR_INVALID_ARG;
    }

    pacote_telemetria_t pacote{};
    pacote.inicio_1 = TELEMETRIA_INICIO_1;
    pacote.inicio_2 = TELEMETRIA_INICIO_2;
    pacote.versao = TELEMETRIA_VERSAO_PROTOCOLO;
    pacote.tipo = TELEMETRIA_TIPO_DADOS;
    pacote.tamanho_carga = TELEMETRIA_TAMANHO_CARGA_DADOS;
    pacote.sequencia = proxima_sequencia++;
    pacote.tempo_mestre_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    pacote.tensao_adc_bruta = dados.tensao_adc_bruta;
    pacote.aceleracao_x = dados.aceleracao_x;
    pacote.aceleracao_y = dados.aceleracao_y;
    pacote.aceleracao_z = dados.aceleracao_z;
    pacote.giroscopio_x = dados.giroscopio_x;
    pacote.giroscopio_y = dados.giroscopio_y;
    pacote.giroscopio_z = dados.giroscopio_z;
    pacote.marcha = static_cast<uint8_t>(dados.marcha) & 0x7Fu;
    if (dados.simulado) pacote.marcha |= 0x80u;
    pacote.velocidade_centesimos_kmh = escalar_uint16(dados.velocidade_kmh, 100.0f);
    pacote.aceleracao_milesimos_ms2 = escalar_int16(dados.aceleracao_ms2, 1000.0f);
    pacote.acelerador_decimos_percentual =
        escalar_uint16(dados.acelerador_percentual, 10.0f);
    pacote.freio_decimos_percentual = escalar_uint16(dados.freio_percentual, 10.0f);
    pacote.tensao_pacote_mv = escalar_uint16(dados.tensao_pacote_v, 1000.0f);
    pacote.corrente_centesimos_a = escalar_int16(dados.corrente_a, 100.0f);
    pacote.carga_decimos_percentual = escalar_uint16(dados.carga_percentual, 10.0f);
    for (size_t indice = 0; indice < 4; ++indice) {
        pacote.tensoes_celulas_mv[indice] =
            escalar_uint16(dados.tensoes_celulas_v[indice], 1000.0f);
    }
    pacote.latitude_micrograus = static_cast<int32_t>(std::llround(
        std::clamp(dados.latitude * 1000000.0, -2147483648.0, 2147483647.0)));
    pacote.longitude_micrograus = static_cast<int32_t>(std::llround(
        std::clamp(dados.longitude * 1000000.0, -2147483648.0, 2147483647.0)));
    pacote.rumo_decimos_grau = escalar_uint16(dados.rumo_graus, 10.0f);
    for (size_t indice = 0; indice < 8; ++indice) {
        pacote.distancias_cm[indice] = escalar_uint16(dados.distancias_cm[indice], 1.0f);
    }
    pacote.potencia_w = escalar_int16(dados.potencia_w, 1.0f);
    pacote.autonomia_decimos_km = escalar_uint16(dados.autonomia_km, 10.0f);
    pacote.percurso_metros = escalar_uint32(dados.percurso_km, 1000.0f);
    pacote.odometro_metros = escalar_uint32(dados.odometro_km, 1000.0f);
    pacote.crc16 = protocolo_telemetria_calcular_crc16(&pacote);

    esp_err_t erro_equipe = gerenciador_uart_enviar(DestinoUart::Equipe, &pacote, sizeof(pacote));
    esp_err_t erro_visitantes =
        gerenciador_uart_enviar(DestinoUart::Visitantes, &pacote, sizeof(pacote));

    portENTER_CRITICAL(&trava_estatisticas);
    ultimos_dados = dados;
    estatisticas.ultima_sequencia = pacote.sequencia;
    estatisticas.ultimo_crc16 = pacote.crc16;
    estatisticas.pacotes_gerados++;
    if (erro_equipe != ESP_OK || erro_visitantes != ESP_OK) {
        estatisticas.pacotes_com_falha++;
    }
    portEXIT_CRITICAL(&trava_estatisticas);

    return erro_equipe != ESP_OK ? erro_equipe : erro_visitantes;
}

EstatisticasTelemetria servico_telemetria_obter_estatisticas() {
    portENTER_CRITICAL(&trava_estatisticas);
    EstatisticasTelemetria copia = estatisticas;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}

DadosTelemetriaVeiculo servico_telemetria_obter_ultimos_dados() {
    portENTER_CRITICAL(&trava_estatisticas);
    const DadosTelemetriaVeiculo copia = ultimos_dados;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}

EstatisticasLinkTelemetria servico_telemetria_obter_estatisticas_link(
    SateliteTelemetria satelite) {
    EstatisticasLinkTelemetria copia{};
    const MonitorConfirmacao* monitor = encontrar_monitor(satelite);
    if (monitor == nullptr) return copia;
    const uint32_t agora_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&trava_estatisticas);
    copia = monitor->estatisticas;
    copia.idade_ultima_confirmacao_ms = monitor->instante_ultima_confirmacao_ms == 0
        ? UINT32_MAX : agora_ms - monitor->instante_ultima_confirmacao_ms;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}

esp_err_t servico_telemetria_definir_intervalo_durante_ota(
    uint32_t intervalo_ms) {
    if (intervalo_ms < configuracao::INTERVALO_TELEMETRIA_OTA_MINIMO_MS ||
        intervalo_ms > configuracao::INTERVALO_TELEMETRIA_OTA_MAXIMO_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&trava_estatisticas);
    intervalo_durante_ota_ms = intervalo_ms;
    portEXIT_CRITICAL(&trava_estatisticas);
    ESP_LOGI(ETIQUETA, "Durante OTA, telemetria a cada %lu ms",
             static_cast<unsigned long>(intervalo_ms));
    return ESP_OK;
}

uint32_t servico_telemetria_obter_intervalo_durante_ota() {
    portENTER_CRITICAL(&trava_estatisticas);
    const uint32_t intervalo = intervalo_durante_ota_ms;
    portEXIT_CRITICAL(&trava_estatisticas);
    return intervalo;
}

uint32_t servico_telemetria_obter_intervalo_atual() {
    portENTER_CRITICAL(&trava_estatisticas);
    const uint32_t intervalo = ota_satelite_ativa
        ? intervalo_durante_ota_ms
        : static_cast<uint32_t>(configuracao::INTERVALO_TELEMETRIA_MS);
    portEXIT_CRITICAL(&trava_estatisticas);
    return intervalo;
}

void servico_telemetria_definir_ota_satelite_ativa(bool ativa) {
    uint32_t intervalo = configuracao::INTERVALO_TELEMETRIA_MS;
    portENTER_CRITICAL(&trava_estatisticas);
    ota_satelite_ativa = ativa;
    if (ativa) intervalo = intervalo_durante_ota_ms;
    portEXIT_CRITICAL(&trava_estatisticas);
    ESP_LOGI(ETIQUETA, "%s: intervalo de telemetria=%lu ms",
             ativa ? "OTA de satélite ativa" : "OTA de satélite encerrada",
             static_cast<unsigned long>(intervalo));
}
