#include "servicos/servico_telemetria.h"

#include "gerenciadores/gerenciador_uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "protocolo_telemetria.h"

namespace {
constexpr char ETIQUETA[] = "servico_telemetria";

uint16_t proxima_sequencia = 0;
EstatisticasTelemetria estatisticas{};
portMUX_TYPE trava_estatisticas = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

esp_err_t servico_telemetria_iniciar() {
    esp_err_t erro = gerenciador_uart_iniciar();
    if (erro == ESP_OK) ESP_LOGI(ETIQUETA, "Servico pronto");
    return erro;
}

esp_err_t servico_telemetria_enviar(uint16_t tensao_bruta,
                                    const LeituraMpu6050& leitura_mpu) {
    if (tensao_bruta > 4095) {
        ESP_LOGE(ETIQUETA, "Pacote rejeitado: ADC fora da faixa (%u)", tensao_bruta);
        return ESP_ERR_INVALID_ARG;
    }

    pacote_telemetria_t pacote{};
    pacote.inicio_1 = TELEMETRIA_INICIO_1;
    pacote.inicio_2 = TELEMETRIA_INICIO_2;
    pacote.sequencia = proxima_sequencia++;
    pacote.tempo_mestre_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    pacote.tensao_adc_bruta = tensao_bruta;
    pacote.aceleracao_x = leitura_mpu.aceleracao_x;
    pacote.aceleracao_y = leitura_mpu.aceleracao_y;
    pacote.aceleracao_z = leitura_mpu.aceleracao_z;
    pacote.giroscopio_x = leitura_mpu.giroscopio_x;
    pacote.giroscopio_y = leitura_mpu.giroscopio_y;
    pacote.giroscopio_z = leitura_mpu.giroscopio_z;
    pacote.checksum = protocolo_telemetria_calcular_checksum(&pacote);

    esp_err_t erro_equipe = gerenciador_uart_enviar(DestinoUart::Equipe, &pacote, sizeof(pacote));
    esp_err_t erro_visitantes =
        gerenciador_uart_enviar(DestinoUart::Visitantes, &pacote, sizeof(pacote));

    portENTER_CRITICAL(&trava_estatisticas);
    estatisticas.ultima_sequencia = pacote.sequencia;
    estatisticas.ultimo_checksum = pacote.checksum;
    estatisticas.pacotes_gerados++;
    if (erro_equipe == ESP_OK) estatisticas.envios_equipe_ok++;
    else estatisticas.erros_equipe++;
    if (erro_visitantes == ESP_OK) estatisticas.envios_visitantes_ok++;
    else estatisticas.erros_visitantes++;
    portEXIT_CRITICAL(&trava_estatisticas);

    return erro_equipe != ESP_OK ? erro_equipe : erro_visitantes;
}

EstatisticasTelemetria servico_telemetria_obter_estatisticas() {
    portENTER_CRITICAL(&trava_estatisticas);
    EstatisticasTelemetria copia = estatisticas;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}
