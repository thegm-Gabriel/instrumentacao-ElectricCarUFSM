// Consolida saúde, falhas e um retrato neutro dos subsistemas.
#include "nucleo/configuracao_tarefas.h"
#include "servicos/servico_diagnostico.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "dispositivos/encoder_incremental.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_i2c.h"
#include "gerenciadores/gerenciador_uart.h"
#include "servicos/servico_odometro.h"
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
DiagnosticoTarefas tarefas_publicadas{};
DiagnosticoTarefas tarefas_coleta{};
TaskStatus_t estados_tarefas[MAXIMO_TAREFAS_DIAGNOSTICO]{};
uint64_t cpu_anterior[MAXIMO_TAREFAS_DIAGNOSTICO]{};
uint32_t ids_anteriores[MAXIMO_TAREFAS_DIAGNOSTICO]{};
uint64_t total_cpu_anterior = 0;
uint32_t menor_pilha_tarefas = 0;
char tarefa_menor_pilha[24]{};
bool pilhas_medidas = false;

void atualizar_tarefas() {
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t quantidade = uxTaskGetSystemState(
        estados_tarefas, MAXIMO_TAREFAS_DIAGNOSTICO, &total);
    tarefas_coleta = {};
    tarefas_coleta.coletado_em_ms = esp_timer_get_time() / 1000;
    tarefas_coleta.valido = quantidade != 0;
    tarefas_coleta.quantidade = quantidade;
    uint32_t menor = UINT32_MAX;
    const char* nome_menor = "-";
    for (UBaseType_t i = 0; i < quantidade; ++i) {
        const auto& origem = estados_tarefas[i];
        auto& destino = tarefas_coleta.tarefas[i];
        std::snprintf(destino.nome, sizeof(destino.nome), "%s", origem.pcTaskName);
        destino.identificador = origem.xTaskNumber;
        destino.prioridade = origem.uxCurrentPriority;
        destino.estado = static_cast<uint8_t>(origem.eCurrentState);
        destino.pilha_minima_livre_bytes = origem.usStackHighWaterMark;
        if (std::strcmp(destino.nome, "telemetria") == 0)
            servico_supervisao_atualizar_pilha(ModuloSupervisionado::TarefaTelemetria, destino.pilha_minima_livre_bytes);
        if (std::strcmp(destino.nome, "enlace_equipe") == 0)
            servico_supervisao_atualizar_pilha(ModuloSupervisionado::UartEquipe, destino.pilha_minima_livre_bytes);
        if (std::strcmp(destino.nome, "enlace_visitantes") == 0)
            servico_supervisao_atualizar_pilha(ModuloSupervisionado::UartVisitantes, destino.pilha_minima_livre_bytes);
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        destino.nucleo = origem.xCoreID == tskNO_AFFINITY ? -1 : origem.xCoreID;
#endif
        const auto* politica = configuracao::politica_tarefa(destino.nome);
        destino.pilha_reservada_bytes =
            politica != nullptr ? politica->pilha_bytes
                                : configuracao::pilha_tarefa_sistema(
                                      destino.nome);
        if (destino.pilha_minima_livre_bytes < menor) {
            menor = destino.pilha_minima_livre_bytes;
            nome_menor = destino.nome;
        }
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        if (total_cpu_anterior != 0 && total > total_cpu_anterior) {
            for (std::size_t j = 0; j < MAXIMO_TAREFAS_DIAGNOSTICO; ++j) {
                if (ids_anteriores[j] != destino.identificador ||
                    origem.ulRunTimeCounter < cpu_anterior[j]) continue;
                const uint64_t uso = (origem.ulRunTimeCounter - cpu_anterior[j]) * 1000ULL /
                                     (total - total_cpu_anterior);
                destino.cpu_decimos = static_cast<uint16_t>(std::min<uint64_t>(uso, 1000));
                destino.cpu_valida = true;
                break;
            }
        }
#endif
    }
    if (quantidade != 0) {
        menor_pilha_tarefas = menor;
        std::snprintf(tarefa_menor_pilha, sizeof(tarefa_menor_pilha), "%s",
                      nome_menor);
        pilhas_medidas = true;
        for (std::size_t i = 0; i < MAXIMO_TAREFAS_DIAGNOSTICO; ++i) {
            ids_anteriores[i] = i < quantidade ? estados_tarefas[i].xTaskNumber : 0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            cpu_anterior[i] = i < quantidade ? estados_tarefas[i].ulRunTimeCounter : 0;
#endif
        }
        total_cpu_anterior = total;
    }
    portENTER_CRITICAL(&trava_diagnostico);
    tarefas_publicadas = tarefas_coleta;
    portEXIT_CRITICAL(&trava_diagnostico);
#endif
}

void registrar_resultado(RegistroErroDiagnostico& registro, esp_err_t resultado,
                         uint64_t instante_ms) {
    registro.ultimo_erro = resultado;
    if (resultado == ESP_OK) {
        registro.consecutivos = 0;
        return;
    }
    registro.total++;
    registro.consecutivos++;
    if (registro.primeira_ocorrencia_ms == 0) {
        registro.primeira_ocorrencia_ms = instante_ms;
    }
    registro.ultima_ocorrencia_ms = instante_ms;
}

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
    resumo.valido = true;
    resumo.tempo_ativo_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    resumo.heap_livre = esp_get_free_heap_size();
    resumo.menor_heap_livre = esp_get_minimum_free_heap_size();
    resumo.bytes_pilha_livres = menor_pilha_tarefas;
    std::snprintf(resumo.tarefa_menor_pilha,
                  sizeof(resumo.tarefa_menor_pilha), "%s",
                  pilhas_medidas ? tarefa_menor_pilha : "-");
    resumo.heap_interno_livre = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    resumo.maior_bloco_heap_interno =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    resumo.psram_livre = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    resumo.maior_bloco_psram =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    resumo.motivo_ultimo_reset = static_cast<uint32_t>(esp_reset_reason());
    resumo.erros_sensores = sensores.adc.total + sensores.mpu6050.total +
                            sensores.encoder.total;
    resumo.erros_i2c = i2c.erros;
    resumo.erros_uart = equipe.erros_envio + equipe.erros_recepcao +
                        visitantes.erros_envio + visitantes.erros_recepcao;
    resumo.descartes_uart =
        equipe.mensagens_descartadas + visitantes.mensagens_descartadas;
    resumo.falhas_ota = ota.falhas;
    resumo.wifi_ativo = wifi.radio_ativo;
    resumo.wifi_conectado = wifi.conectado;
    resumo.wifi_rssi_dbm = wifi.rssi_dbm;
    const uint32_t limite_confirmacao_ms =
        servico_telemetria_obter_intervalo_atual() * 3u + 500u;
    resumo.equipe_respondendo =
        link_equipe.respondeu &&
        link_equipe.idade_ultima_confirmacao_ms <= limite_confirmacao_ms;
    resumo.visitantes_respondendo =
        link_visitantes.respondeu &&
        link_visitantes.idade_ultima_confirmacao_ms <= limite_confirmacao_ms;
    resumo.latencia_equipe_ms = link_equipe.latencia_atual_ms;
    resumo.latencia_visitantes_ms = link_visitantes.latencia_atual_ms;
    resumo.falhas_supervisao = supervisao_telemetria.falhas_detectadas +
                               supervisao_equipe.falhas_detectadas +
                               supervisao_visitantes.falhas_detectadas;

    const int64_t instante_ms = esp_timer_get_time() / 1000;
    uint32_t causas = CausaSaudeNenhuma;
    if (resumo.heap_interno_livre < 24U * 1024U ||
        resumo.maior_bloco_heap_interno < 8192) {
        causas |= CausaSaudeHeapCritico;
    } else if (resumo.heap_interno_livre < 48U * 1024U ||
               resumo.maior_bloco_heap_interno < 20U * 1024U) {
        causas |= CausaSaudeMemoriaBaixa;
    }
    if (pilhas_medidas &&
        resumo.bytes_pilha_livres < 512) {
        causas |= CausaSaudePilhaCritica;
    }
    if (sensores.adc.consecutivos != 0 ||
        sensores.mpu6050.consecutivos != 0 ||
        sensores.encoder.consecutivos != 0) causas |= CausaSaudeSensor;
    if (i2c.ultimo_erro != ESP_OK) causas |= CausaSaudeI2c;
    if (sensores.persistencia_odometro.consecutivos != 0) {
        causas |= CausaSaudePersistencia;
    }
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
    for (unsigned i = static_cast<unsigned>(ModuloSupervisionado::Aceleracao);
         i < static_cast<unsigned>(ModuloSupervisionado::Quantidade); ++i) {
        const auto modulo = servico_supervisao_obter_estado(static_cast<ModuloSupervisionado>(i));
        resumo.falhas_supervisao += modulo.falhas_detectadas;
        if (modulo.atrasado) causas |= CausaSaudeSupervisao;
    }
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
    uint32_t ciclos = 0;
    while (true) {
        if (ciclos++ % 5 == 0) atualizar_tarefas();
        const ResumoSaudeSistema novo = calcular_resumo();
        portENTER_CRITICAL(&trava_diagnostico);
        resumo_publicado = novo;
        portEXIT_CRITICAL(&trava_diagnostico);
        vTaskDelay(pdMS_TO_TICKS(INTERVALO_ATUALIZACAO_MS));
    }
}
}  // namespace

void servico_diagnostico_obter_tarefas(DiagnosticoTarefas* destino) {
    if (destino == nullptr) return;
    portENTER_CRITICAL(&trava_diagnostico);
    *destino = tarefas_publicadas;
    portEXIT_CRITICAL(&trava_diagnostico);
}

esp_err_t servico_diagnostico_iniciar() {
    if (tarefa_diagnostico != nullptr) return ESP_OK;
    const ResumoSaudeSistema inicial = calcular_resumo();
    portENTER_CRITICAL(&trava_diagnostico);
    resumo_publicado = inicial;
    portEXIT_CRITICAL(&trava_diagnostico);
    if (configuracao::criar_tarefa(tarefa_atualizacao, "diagnostico", nullptr, &tarefa_diagnostico) != pdPASS) {
        tarefa_diagnostico = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA, "Saúde do sistema atualizada a cada %lu ms",
             static_cast<unsigned long>(INTERVALO_ATUALIZACAO_MS));
    return ESP_OK;
}

esp_err_t servico_diagnostico_registrar_resultado(
    SensorDiagnosticado sensor, esp_err_t resultado) {
    RegistroErroDiagnostico* registro = nullptr;
    switch (sensor) {
        case SensorDiagnosticado::Adc: registro = &diagnostico.adc; break;
        case SensorDiagnosticado::Mpu6050: registro = &diagnostico.mpu6050; break;
        case SensorDiagnosticado::Encoder: registro = &diagnostico.encoder; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    const uint64_t instante_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&trava_diagnostico);
    registrar_resultado(*registro, resultado, instante_ms);
    portEXIT_CRITICAL(&trava_diagnostico);
    return ESP_OK;
}

EstatisticasSensoresDiagnostico servico_diagnostico_obter_sensores() {
    const EstadoOdometro odometro = servico_odometro_obter_estado();
    portENTER_CRITICAL(&trava_diagnostico);
    EstatisticasSensoresDiagnostico copia = diagnostico;
    portEXIT_CRITICAL(&trava_diagnostico);
    copia.encoder.total += odometro.erros_leitura + odometro.erros_validacao +
                           odometro.divergencias_detectadas;
    copia.persistencia_odometro.total = odometro.erros_persistencia;
    copia.persistencia_odometro.ultimo_erro = odometro.ultimo_erro_persistencia;
    copia.persistencia_odometro.consecutivos =
        odometro.ultimo_erro_persistencia == ESP_OK ? 0 : 1;
    if (copia.encoder.ultimo_erro == ESP_OK &&
        odometro.ultimo_erro_leitura != ESP_OK) {
        copia.encoder.ultimo_erro = odometro.ultimo_erro_leitura;
        copia.encoder.consecutivos = 1;
    }
    if (copia.encoder.ultimo_erro == ESP_OK &&
        odometro.ultimo_erro_validacao != ESP_OK) {
        copia.encoder.ultimo_erro = odometro.ultimo_erro_validacao;
        copia.encoder.consecutivos = 1;
    }
    if (!odometro.rodas_coerentes) {
        copia.encoder.ultimo_erro = ESP_ERR_INVALID_RESPONSE;
        copia.encoder.consecutivos = 1;
    }
    return copia;
}

ResumoSaudeSistema servico_diagnostico_obter_resumo() {
    portENTER_CRITICAL(&trava_diagnostico);
    const ResumoSaudeSistema copia = resumo_publicado;
    portEXIT_CRITICAL(&trava_diagnostico);
    return copia;
}

RetratoDiagnosticoSistema servico_diagnostico_obter_retrato() {
    RetratoDiagnosticoSistema retrato{};
    retrato.coletado_em_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    retrato.telemetria_atual = servico_telemetria_obter_ultimos_dados();
    const EstatisticasTelemetria telemetria =
        servico_telemetria_obter_estatisticas();
    retrato.telemetria.ultima_sequencia = telemetria.ultima_sequencia;
    retrato.telemetria.pacotes_gerados = telemetria.pacotes_gerados;
    retrato.telemetria.pacotes_com_falha = telemetria.pacotes_com_falha;
    retrato.telemetria.ultimo_crc16 = telemetria.ultimo_crc16;
    retrato.sensores = servico_diagnostico_obter_sensores();
    retrato.saude = servico_diagnostico_obter_resumo();
    const EstatisticasI2c i2c = gerenciador_i2c_obter_estatisticas();
    retrato.i2c = {i2c.operacoes_ok, i2c.erros, i2c.sondagens_ok,
                   i2c.enderecos_ausentes, i2c.ultimo_erro};

    const auto copiar_supervisao = [](const EstadoModuloSupervisionado& origem) {
        return DiagnosticoSupervisao{origem.idade_ultimo_pulso_ms,
                                     origem.falhas_detectadas,
                                     origem.recuperacoes_ok};
    };
    retrato.supervisao_telemetria = copiar_supervisao(
        servico_supervisao_obter_estado(ModuloSupervisionado::TarefaTelemetria));
    retrato.supervisao_equipe = copiar_supervisao(
        servico_supervisao_obter_estado(ModuloSupervisionado::UartEquipe));
    retrato.supervisao_visitantes = copiar_supervisao(
        servico_supervisao_obter_estado(ModuloSupervisionado::UartVisitantes));

    const auto copiar_encoder = [](const EstadoEncoderIncremental& origem) {
        DiagnosticoEncoder destino{};
        destino.inicializado = origem.inicializado;
        destino.pino_a = static_cast<int>(origem.pino_a);
        destino.pino_b = static_cast<int>(origem.pino_b);
        destino.contagens_por_giro = origem.contagens_por_giro;
        destino.contagem_atual = origem.contagem_atual;
        destino.transbordamentos_compensados = origem.transbordamentos_compensados;
        destino.leituras_ok = origem.leituras_ok;
        destino.erros_leitura = origem.erros_leitura;
        return destino;
    };
    retrato.encoder_esquerdo = copiar_encoder(encoder_incremental_obter_estado(
        EncoderIncrementalId::RodaEsquerda));
    retrato.encoder_direito = copiar_encoder(encoder_incremental_obter_estado(
        EncoderIncrementalId::RodaDireita));

    const EstadoOdometro odometro = servico_odometro_obter_estado();
    retrato.odometro.roda_esquerda = {
        odometro.roda_esquerda.contagens_acumuladas,
        odometro.roda_esquerda.percurso_m,
        odometro.roda_esquerda.velocidade_kmh,
        odometro.roda_esquerda.metros_por_contagem};
    retrato.odometro.roda_direita = {
        odometro.roda_direita.contagens_acumuladas,
        odometro.roda_direita.percurso_m,
        odometro.roda_direita.velocidade_kmh,
        odometro.roda_direita.metros_por_contagem};
    retrato.odometro.percurso_m = odometro.percurso_m;
    retrato.odometro.odometro_m = odometro.odometro_m;
    retrato.odometro.velocidade_kmh = odometro.velocidade_kmh;
    retrato.odometro.erros_persistencia = odometro.erros_persistencia;
    retrato.odometro.erros_validacao = odometro.erros_validacao;
    retrato.odometro.divergencias_detectadas = odometro.divergencias_detectadas;
    retrato.odometro.rodas_coerentes = odometro.rodas_coerentes;
    retrato.odometro.persistencia_disponivel = odometro.persistencia_disponivel;
    retrato.odometro.persistencia_pendente = odometro.persistencia_pendente;

    const ResumoWifi wifi = servico_wifi_obter_resumo();
    retrato.wifi.radio_ativo = wifi.radio_ativo;
    retrato.wifi.conectado = wifi.conectado;
    std::strncpy(retrato.wifi.rede_ativa, wifi.rede_ativa,
                 sizeof(retrato.wifi.rede_ativa) - 1);
    retrato.wifi.rssi_dbm = wifi.rssi_dbm;
    retrato.wifi.canal = wifi.canal;
    retrato.wifi.ultimo_motivo_desconexao = wifi.ultimo_motivo_desconexao;
    retrato.wifi.ultimo_erro = wifi.ultimo_erro;
    return retrato;
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
