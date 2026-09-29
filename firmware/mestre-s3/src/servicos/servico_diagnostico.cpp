#include "servicos/servico_diagnostico.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_i2c.h"
#include "gerenciadores/gerenciador_uart.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_supervisao.h"
#include "servicos/servico_telemetria.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "diagnostico";
constexpr uint32_t INTERVALO_ATUALIZACAO_MS = 1000;

struct MemoriaEstadoSaude {
    uint32_t erros_uart_observados = 0;
    uint32_t descartes_uart_observados = 0;
    uint32_t falhas_ota_observadas = 0;
    int64_t alerta_uart_ate_ms = 0;
    int64_t alerta_ota_ate_ms = 0;
    int64_t recuperando_ate_ms = 0;
};

EstatisticasSensoresDiagnostico diagnostico{};
MemoriaEstadoSaude memoria_saude{};
ResumoSaudeSistema resumo_publicado{};
portMUX_TYPE trava_diagnostico = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t tarefa_diagnostico = nullptr;

ResumoSaudeSistema calcular_resumo() {
    const EstatisticasSensoresDiagnostico sensores =
        servico_diagnostico_obter_sensores();
    const EstatisticasI2c i2c = gerenciador_i2c_obter_estatisticas();
    const EstatisticasUart equipe =
        gerenciador_uart_obter_estatisticas(DestinoUart::Equipe);
    const EstatisticasUart visitantes =
        gerenciador_uart_obter_estatisticas(DestinoUart::Visitantes);
    const SituacaoOta ota = servico_ota_obter_situacao();
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    const EstatisticasTelemetria telemetria =
        servico_telemetria_obter_estatisticas();
    const EstatisticasLinkTelemetria link_equipe =
        servico_telemetria_obter_estatisticas_link(SateliteTelemetria::Equipe);
    const EstatisticasLinkTelemetria link_visitantes =
        servico_telemetria_obter_estatisticas_link(SateliteTelemetria::Visitantes);
    const EstadoModuloSupervisionado supervisao_telemetria =
        servico_supervisao_obter_estado(ModuloSupervisionado::TarefaTelemetria);
    const EstadoModuloSupervisionado supervisao_equipe =
        servico_supervisao_obter_estado(ModuloSupervisionado::UartEquipe);
    const EstadoModuloSupervisionado supervisao_visitantes =
        servico_supervisao_obter_estado(ModuloSupervisionado::UartVisitantes);

    ResumoSaudeSistema resumo{};
    resumo.tempo_ativo_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    resumo.heap_livre = esp_get_free_heap_size();
    resumo.menor_heap_livre = esp_get_minimum_free_heap_size();
    resumo.palavras_pilha_livres =
        servico_supervisao_obter_menor_pilha_livre();
    resumo.motivo_ultimo_reset = static_cast<uint32_t>(esp_reset_reason());
    resumo.erros_sensores = sensores.erros_adc + sensores.erros_mpu6050;
    resumo.erros_i2c = i2c.erros;
    resumo.erros_uart = equipe.erros_envio + equipe.erros_recepcao +
                        visitantes.erros_envio + visitantes.erros_recepcao;
    resumo.descartes_uart =
        equipe.mensagens_descartadas + visitantes.mensagens_descartadas;
    resumo.falhas_ota = ota.falhas;
    resumo.wifi_ativo = wifi.radio_ativo;
    resumo.wifi_conectado = wifi.conectado;
    resumo.wifi_rssi_dbm = wifi.rssi_dbm;
    resumo.equipe_respondendo = link_equipe.respondeu &&
                                link_equipe.idade_ultima_confirmacao_ms <= 3000u;
    resumo.visitantes_respondendo = link_visitantes.respondeu &&
                                    link_visitantes.idade_ultima_confirmacao_ms <= 3000u;
    resumo.latencia_equipe_ms = link_equipe.latencia_atual_ms;
    resumo.latencia_visitantes_ms = link_visitantes.latencia_atual_ms;
    resumo.falhas_supervisao = supervisao_telemetria.falhas_detectadas +
                               supervisao_equipe.falhas_detectadas +
                               supervisao_visitantes.falhas_detectadas;

    const int64_t instante_ms = esp_timer_get_time() / 1000;
    uint32_t causas = CausaSaudeNenhuma;
    if (resumo.heap_livre < 20000) causas |= CausaSaudeHeapCritico;
    if (resumo.palavras_pilha_livres != 0 &&
        resumo.palavras_pilha_livres < 128) {
        causas |= CausaSaudePilhaCritica;
    }
    if (sensores.ultimo_erro_adc != ESP_OK ||
        sensores.ultimo_erro_mpu6050 != ESP_OK) causas |= CausaSaudeSensor;
    if (i2c.ultimo_erro != ESP_OK) causas |= CausaSaudeI2c;
    // Os últimos códigos permanecem disponíveis para diagnóstico, mas não
    // mantêm a saúde em atenção para sempre. O contador abaixo abre uma janela
    // temporária e erros recorrentes renovam essa janela.
    const bool supervisao_ativa = supervisao_telemetria.em_recuperacao ||
        supervisao_equipe.em_recuperacao || supervisao_visitantes.em_recuperacao ||
        (supervisao_telemetria.registrado &&
         supervisao_telemetria.idade_ultimo_pulso_ms >
             supervisao_telemetria.tempo_limite_ms) ||
        (supervisao_equipe.registrado &&
         supervisao_equipe.idade_ultimo_pulso_ms >
             supervisao_equipe.tempo_limite_ms) ||
        (supervisao_visitantes.registrado &&
         supervisao_visitantes.idade_ultimo_pulso_ms >
             supervisao_visitantes.tempo_limite_ms);
    if (supervisao_ativa) causas |= CausaSaudeSupervisao;
    if (telemetria.pacotes_gerados >= 10u && !resumo.equipe_respondendo)
        causas |= CausaSaudeEquipeSemResposta;
    if (telemetria.pacotes_gerados >= 10u && !resumo.visitantes_respondendo)
        causas |= CausaSaudeVisitantesSemResposta;
    if (wifi.conectado && wifi.rssi_dbm < -80) causas |= CausaSaudeWifiFraco;

    portENTER_CRITICAL(&trava_diagnostico);
    if (resumo.erros_uart > memoria_saude.erros_uart_observados ||
        resumo.descartes_uart > memoria_saude.descartes_uart_observados) {
        memoria_saude.alerta_uart_ate_ms = instante_ms + 5000;
    }
    if (resumo.falhas_ota > memoria_saude.falhas_ota_observadas) {
        memoria_saude.alerta_ota_ate_ms = instante_ms + 10000;
    }
    memoria_saude.erros_uart_observados = resumo.erros_uart;
    memoria_saude.descartes_uart_observados = resumo.descartes_uart;
    memoria_saude.falhas_ota_observadas = resumo.falhas_ota;
    if (instante_ms < memoria_saude.alerta_uart_ate_ms) causas |= CausaSaudeUart;
    if (instante_ms < memoria_saude.alerta_ota_ate_ms)
        causas |= CausaSaudeOtaRecente;
    if (causas != CausaSaudeNenhuma) {
        memoria_saude.recuperando_ate_ms = instante_ms + 2000;
    }
    const bool recuperando = causas == CausaSaudeNenhuma &&
                             instante_ms < memoria_saude.recuperando_ate_ms;
    portEXIT_CRITICAL(&trava_diagnostico);

    resumo.causas_ativas = causas;
    const uint32_t causas_criticas =
        CausaSaudeHeapCritico | CausaSaudePilhaCritica;
    if ((causas & causas_criticas) != 0) {
        resumo.estado = EstadoSaudeSistema::Falha;
    } else if (causas != CausaSaudeNenhuma) {
        resumo.estado = EstadoSaudeSistema::Atencao;
    } else if (recuperando) {
        resumo.estado = EstadoSaudeSistema::Recuperando;
    }
    return resumo;
}

void tarefa_atualizacao(void*) {
    while (true) {
        const ResumoSaudeSistema novo = calcular_resumo();
        portENTER_CRITICAL(&trava_diagnostico);
        resumo_publicado = novo;
        portEXIT_CRITICAL(&trava_diagnostico);
        vTaskDelay(pdMS_TO_TICKS(INTERVALO_ATUALIZACAO_MS));
    }
}
}  // namespace

esp_err_t servico_diagnostico_iniciar() {
    if (tarefa_diagnostico != nullptr) return ESP_OK;
    const ResumoSaudeSistema inicial = calcular_resumo();
    portENTER_CRITICAL(&trava_diagnostico);
    resumo_publicado = inicial;
    portEXIT_CRITICAL(&trava_diagnostico);
    if (xTaskCreate(tarefa_atualizacao, "diagnostico", 6144, nullptr, 4,
                    &tarefa_diagnostico) != pdPASS) {
        tarefa_diagnostico = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA, "Saúde do sistema atualizada a cada %lu ms",
             static_cast<unsigned long>(INTERVALO_ATUALIZACAO_MS));
    return ESP_OK;
}

void servico_diagnostico_registrar_erro_adc(esp_err_t erro) {
    portENTER_CRITICAL(&trava_diagnostico);
    diagnostico.ultimo_erro_adc = erro;
    if (erro != ESP_OK) diagnostico.erros_adc++;
    portEXIT_CRITICAL(&trava_diagnostico);
}

void servico_diagnostico_registrar_erro_mpu6050(esp_err_t erro) {
    portENTER_CRITICAL(&trava_diagnostico);
    diagnostico.ultimo_erro_mpu6050 = erro;
    if (erro != ESP_OK) diagnostico.erros_mpu6050++;
    portEXIT_CRITICAL(&trava_diagnostico);
}

EstatisticasSensoresDiagnostico servico_diagnostico_obter_sensores() {
    portENTER_CRITICAL(&trava_diagnostico);
    const EstatisticasSensoresDiagnostico copia = diagnostico;
    portEXIT_CRITICAL(&trava_diagnostico);
    return copia;
}

ResumoSaudeSistema servico_diagnostico_obter_resumo() {
    portENTER_CRITICAL(&trava_diagnostico);
    const ResumoSaudeSistema copia = resumo_publicado;
    portEXIT_CRITICAL(&trava_diagnostico);
    return copia;
}

const char* servico_diagnostico_nome_estado(EstadoSaudeSistema estado) {
    switch (estado) {
        case EstadoSaudeSistema::Saudavel: return "saudável";
        case EstadoSaudeSistema::Recuperando: return "recuperando";
        case EstadoSaudeSistema::Atencao: return "atenção";
        case EstadoSaudeSistema::Falha: return "falha";
    }
    return "desconhecido";
}
