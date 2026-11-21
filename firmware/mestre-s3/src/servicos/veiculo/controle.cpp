#include "servicos/veiculo/controle.h"

#include <algorithm>
#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "dispositivos/entradas/adc.h"
#include "dispositivos/saidas/mcp4725.h"
#include "dispositivos/saidas/gpio_veiculo.h"
#include "dispositivos/entradas/chave_marcha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao/tarefas.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/veiculo.h"
#include "servicos/veiculo/aquisicao.h"
#include "servicos/veiculo/aceleracao.h"
#include "servicos/veiculo/odometro.h"
#include "servicos/veiculo/simulador.h"
#include "servicos/interface_local/sinalizacao.h"

namespace {
constexpr char ETIQUETA[] = "controle_remoto";
constexpr char ESPACO_NVS_CONTROLE[] = "controle";
constexpr char CHAVE_VELOCIDADE[] = "velocidade";
constexpr char CHAVE_ACELERADOR[] = "acelerador";
static_assert(configuracao::DAC_TENSAO_MINIMA_SAIDA_MV <=
              CONTROLE_REMOTO_TENSAO_MINIMA_MAXIMA_MV);

portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t tarefa_controle = nullptr;
bool iniciado = false;
int64_t parada_expira_em_us = 0;
int64_t rearme_disponivel_em_us = 0;
bool rearme_somente_manual = false;
uint32_t pedal_recebido_em_ms = 0;
bool exige_pedal_zero = true;
uint32_t pedal_zero_desde_ms = 0;
bool armamento_inicial_pendente = true;
uint8_t marcha_candidata = FAIXA_MARCHA_INDEFINIDA;
uint8_t ultima_marcha_confirmada = FAIXA_MARCHA_INDEFINIDA;
uint32_t marcha_candidata_desde_ms = 0;
uint32_t ultima_sondagem_dac_ms = 0;
uint16_t ultima_saida_dac_mv = UINT16_MAX;
uint32_t ultima_escrita_dac_ms = 0;
uint32_t falhas_pedal = 0;
bool falha_saidas = false;
uint32_t ultima_leitura_aceleracao = 0;
uint32_t ultima_leitura_velocidade = 0;
uint32_t aceleracao_recebida_em_ms = 0;
uint32_t velocidade_recebida_em_ms = 0;
estado_controle_remoto_t estado = [] {
    estado_controle_remoto_t seguro{};
    seguro.formato = CONTROLE_REMOTO_FORMATO;
    seguro.faixa_marcha_maxima = FAIXA_MARCHA_L;
    seguro.parada_solicitada = 1;
    seguro.corte_por_rele_solicitado = 1;
    seguro.estado_operacao = CONTROLE_ESTADO_PARADO;
    seguro.armamento_inicial_pendente = 1;
    seguro.zona_morta_decimos_percentual = 50; // 5% do curso medido
    seguro.tensao_minima_saida_mv = configuracao::DAC_TENSAO_MINIMA_SAIDA_MV;
    return seguro;
}();

uint32_t agora_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

uint32_t restante_ms(int64_t prazo_us, int64_t agora_us) {
    const int64_t restante_us = prazo_us - agora_us;
    return restante_us > 0 ? static_cast<uint32_t>((restante_us + 999) / 1000) : 0;
}

void carregar_limites() {
    nvs_handle_t armazenamento;
    if (nvs_open(ESPACO_NVS_CONTROLE, NVS_READONLY, &armazenamento) != ESP_OK)
        return;
    uint32_t velocidade = 0;
    uint32_t acelerador = 0;
    if (nvs_get_u32(armazenamento, CHAVE_VELOCIDADE, &velocidade) == ESP_OK &&
        velocidade <= CONTROLE_REMOTO_VELOCIDADE_MAXIMA)
        estado.velocidade_maxima_centesimos_kmh = velocidade;
    if (nvs_get_u32(armazenamento, CHAVE_ACELERADOR, &acelerador) == ESP_OK &&
        acelerador <= CONTROLE_REMOTO_ACELERADOR_MAXIMO)
        estado.acelerador_maximo_decimos_percentual =
            static_cast<uint16_t>(acelerador);
    nvs_close(armazenamento);
}

esp_err_t salvar_limite(const char* chave, uint32_t valor) {
    nvs_handle_t armazenamento;
    esp_err_t erro = nvs_open(ESPACO_NVS_CONTROLE, NVS_READWRITE,
                             &armazenamento);
    if (erro != ESP_OK) return erro;
    erro = nvs_set_u32(armazenamento, chave, valor);
    if (erro == ESP_OK) erro = nvs_commit(armazenamento);
    nvs_close(armazenamento);
    return erro;
}

void cancelar_acelerador() {
    estado.acelerador_lido_decimos_percentual = 0;
    estado.acelerador_permitido_decimos_percentual = 0;
    pedal_recebido_em_ms = 0;
    exige_pedal_zero = true;
    pedal_zero_desde_ms = 0;
}

void solicitar_parada(uint32_t agora);

void informar_falha_pedal(uint32_t agora, esp_err_t erro) {
    portENTER_CRITICAL(&trava);
    const bool pedal_ativo = estado.acelerador_lido_decimos_percentual != 0 ||
        estado.acelerador_permitido_decimos_percentual != 0;
    estado.pedal_disponivel = 0;
    estado.tensao_pedal_mv = 0;
    if (pedal_ativo) solicitar_parada(agora);
    else cancelar_acelerador();
    portEXIT_CRITICAL(&trava);
    if (++falhas_pedal == 1 || falhas_pedal % 250 == 0)
        ESP_LOGE(ETIQUETA, "Leitura do pedal indisponível: %s (%lu ocorrências)",
                 esp_err_to_name(erro), static_cast<unsigned long>(falhas_pedal));
}

// A parada zera imediatamente a referência de acelerador no mestre.
void solicitar_parada(uint32_t agora) {
    const int64_t inicio_parada_us = esp_timer_get_time();
    armamento_inicial_pendente = false;
    estado.armamento_inicial_pendente = 0;
    cancelar_acelerador();
    estado.faixa_marcha_solicitada = FAIXA_MARCHA_INDEFINIDA;
    estado.reversao_solicitada = 0;
    estado.reversao_acionar_solicitado = 0;
    estado.modo_a_aterrar_solicitado = 0;
    estado.modo_b_aterrar_solicitado = 0;
    estado.parada_solicitada = 1;
    estado.corte_por_rele_solicitado = 1;
    estado.estado_operacao = CONTROLE_ESTADO_PARADO;
    rearme_disponivel_em_us = inicio_parada_us +
        CONTROLE_REMOTO_REARME_MINIMO_MS * 1000LL;
    parada_expira_em_us = rearme_somente_manual ? 0 : inicio_parada_us +
        CONTROLE_REMOTO_REARME_AUTOMATICO_MS * 1000LL;
    ++estado.revisao;
    estado.alterado_em_ms = agora;
}

void liberar_parada(uint32_t agora) {
    armamento_inicial_pendente = false;
    estado.armamento_inicial_pendente = 0;
    cancelar_acelerador();
    // Mesmo após 30 s ou liberação manual, uma faixa antiga nunca é retomada.
    estado.faixa_marcha_solicitada = FAIXA_MARCHA_INDEFINIDA;
    estado.reversao_solicitada = 0;
    estado.parada_solicitada = 0;
    parada_expira_em_us = 0;
    rearme_disponivel_em_us = 0;
    rearme_somente_manual = false;
    ++estado.revisao;
    estado.alterado_em_ms = agora;
}

bool leitura_valida(const EstadoServicoAceleracao& aceleracao,
                    const EstadoOdometro& odometro, uint32_t agora) {
    const bool mpu = aceleracao.iniciado && aceleracao.calibrado &&
        aceleracao.eixo_longitudinal_compativel &&
        aceleracao.ultimo_erro == ESP_OK &&
        std::isfinite(aceleracao.aceleracao_longitudinal_ms2) &&
        aceleracao.leituras_ok > 0 &&
        aceleracao_recebida_em_ms != 0 &&
        agora - aceleracao_recebida_em_ms <= configuracao::PRAZO_ACELERACAO_MS;
    const bool rodas = odometro.iniciado && odometro.rodas_coerentes &&
        odometro.ultimo_erro_leitura == ESP_OK &&
        odometro.ultimo_erro_validacao == ESP_OK &&
        std::isfinite(odometro.velocidade_kmh) &&
        odometro.velocidade_kmh >= 0.0f && odometro.atualizacoes > 0 &&
        velocidade_recebida_em_ms != 0 &&
        agora - velocidade_recebida_em_ms <= configuracao::PRAZO_VELOCIDADE_MS;
    return mpu && rodas;
}

void atualizar_logica(uint32_t agora, const EstadoServicoAceleracao& aceleracao,
                     const EstadoOdometro& odometro, bool simulado,
                     uint8_t marcha_lida) {
    portENTER_CRITICAL(&trava);
    const uint8_t bloqueios_anteriores = estado.bloqueios;
    bool armado_agora = false;
    if (aceleracao.leituras_ok != ultima_leitura_aceleracao) {
        ultima_leitura_aceleracao = aceleracao.leituras_ok;
        aceleracao_recebida_em_ms = agora;
    }
    if (odometro.atualizacoes != ultima_leitura_velocidade) {
        ultima_leitura_velocidade = odometro.atualizacoes;
        velocidade_recebida_em_ms = agora;
    }
    if (std::isfinite(aceleracao.aceleracao_longitudinal_ms2))
        estado.aceleracao_medida_milesimos_ms2 = static_cast<int32_t>(
            std::lround(std::clamp(aceleracao.aceleracao_longitudinal_ms2,
                                   -100.0f, 100.0f) * 1000.0f));
    if (std::isfinite(odometro.velocidade_kmh) && odometro.velocidade_kmh >= 0)
        estado.velocidade_medida_centesimos_kmh = static_cast<uint32_t>(
            std::lround(std::min(odometro.velocidade_kmh, 1000.0f) * 100.0f));

    if (marcha_lida != marcha_candidata) {
        marcha_candidata = marcha_lida;
        marcha_candidata_desde_ms = agora;
        if (marcha_lida == FAIXA_MARCHA_INDEFINIDA &&
            ultima_marcha_confirmada != FAIXA_MARCHA_INDEFINIDA)
            solicitar_parada(agora);
        estado.faixa_marcha_fisica = FAIXA_MARCHA_INDEFINIDA;
        estado.faixa_marcha_solicitada = FAIXA_MARCHA_INDEFINIDA;
    } else if (marcha_lida == FAIXA_MARCHA_INDEFINIDA) {
        estado.faixa_marcha_fisica = FAIXA_MARCHA_INDEFINIDA;
        estado.faixa_marcha_solicitada = FAIXA_MARCHA_INDEFINIDA;
        ultima_marcha_confirmada = FAIXA_MARCHA_INDEFINIDA;
    } else if (agora - marcha_candidata_desde_ms >=
               configuracao::ESTABILIZACAO_CHAVE_MARCHA_MS) {
        const uint8_t efetiva = std::min(marcha_lida, estado.faixa_marcha_maxima);
        if (ultima_marcha_confirmada != FAIXA_MARCHA_INDEFINIDA &&
            ultima_marcha_confirmada != efetiva &&
            (estado.acelerador_lido_decimos_percentual != 0 ||
             estado.velocidade_medida_centesimos_kmh >
                 configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS))
            solicitar_parada(agora);
        estado.faixa_marcha_fisica = marcha_lida;
        estado.faixa_marcha_solicitada = efetiva;
        ultima_marcha_confirmada = efetiva;
    }

    const bool sensores_validos = leitura_valida(aceleracao, odometro, agora);
    bool pedal_recente = pedal_recebido_em_ms != 0 &&
        agora - pedal_recebido_em_ms <= CONTROLE_REMOTO_EXPIRACAO_PEDAL_MS;
    const bool seguro_para_rearmar = estado.partida_concluida &&
        !estado.ota_em_andamento && !falha_saidas && sensores_validos &&
        !simulado && pedal_recente && estado.pedal_disponivel &&
        !exige_pedal_zero && pedal_zero_desde_ms != 0 &&
        agora - pedal_zero_desde_ms >= configuracao::PEDAL_SOLTO_PARA_ARMAR_MS &&
        estado.acelerador_lido_decimos_percentual == 0 &&
        estado.faixa_marcha_fisica != FAIXA_MARCHA_INDEFINIDA &&
        estado.velocidade_medida_centesimos_kmh <=
            configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS &&
        estado.velocidade_maxima_centesimos_kmh >
            estado.velocidade_medida_centesimos_kmh &&
        estado.acelerador_maximo_decimos_percentual > 0 &&
        (!configuracao::HABILITAR_SAIDA_ACELERADOR_DAC ||
         estado.dac_zero_confirmado);
    const int64_t agora_rearme_us = esp_timer_get_time();
    if (seguro_para_rearmar && estado.parada_solicitada &&
        agora_rearme_us >= rearme_disponivel_em_us &&
        (armamento_inicial_pendente ||
         (parada_expira_em_us != 0 && agora_rearme_us >= parada_expira_em_us))) {
        const bool partida_automatica = armamento_inicial_pendente;
        liberar_parada(agora);
        estado.faixa_marcha_solicitada = std::min(
            estado.faixa_marcha_fisica, estado.faixa_marcha_maxima);
        pedal_recente = false; // O pedal deve voltar a zero após o rearme.
        armado_agora = partida_automatica;
    }
    uint8_t bloqueios = 0;
    if (estado.faixa_marcha_solicitada == FAIXA_MARCHA_INDEFINIDA)
        bloqueios |= CONTROLE_BLOQUEIO_SEM_FAIXA;
    if (estado.velocidade_maxima_centesimos_kmh == 0)
        bloqueios |= CONTROLE_BLOQUEIO_SEM_LIMITES;
    if (estado.acelerador_maximo_decimos_percentual == 0)
        bloqueios |= CONTROLE_BLOQUEIO_SEM_LIMITES;
    if (!sensores_validos) bloqueios |= CONTROLE_BLOQUEIO_SEM_SENSORES;
    if (simulado) bloqueios |= CONTROLE_BLOQUEIO_TELEMETRIA_SIMULADA;
    if (falha_saidas) bloqueios |= CONTROLE_BLOQUEIO_FALHA_TRAVADA;
    if (!estado.partida_concluida || estado.ota_em_andamento)
        bloqueios |= CONTROLE_BLOQUEIO_PARTIDA_OU_OTA;
    if (!pedal_recente || exige_pedal_zero)
        bloqueios |= CONTROLE_BLOQUEIO_PEDAL_INDISPONIVEL;
    if (estado.velocidade_maxima_centesimos_kmh != 0 &&
        estado.velocidade_medida_centesimos_kmh >=
            estado.velocidade_maxima_centesimos_kmh)
        bloqueios |= CONTROLE_BLOQUEIO_VELOCIDADE;

    if (estado.acelerador_lido_decimos_percentual != 0 &&
        !pedal_recente) {
        solicitar_parada(agora);
        bloqueios |= CONTROLE_BLOQUEIO_PEDAL_INDISPONIVEL;
    } else if (estado.acelerador_lido_decimos_percentual != 0 &&
               (bloqueios & (CONTROLE_BLOQUEIO_SEM_SENSORES |
                             CONTROLE_BLOQUEIO_TELEMETRIA_SIMULADA))) {
        solicitar_parada(agora);
        bloqueios |= CONTROLE_BLOQUEIO_FALHA_TRAVADA;
    } else if (estado.acelerador_lido_decimos_percentual != 0 &&
        (bloqueios & CONTROLE_BLOQUEIO_VELOCIDADE)) {
        // O pedal precisa voltar a zero após atingir o limite.
        cancelar_acelerador();
    }

    estado.bloqueios = bloqueios;
    if (estado.parada_solicitada || bloqueios != 0) {
        estado.corte_por_rele_solicitado = 1;
        estado.modo_a_aterrar_solicitado = 0;
        estado.modo_b_aterrar_solicitado = 0;
        estado.reversao_acionar_solicitado = 0;
        estado.acelerador_permitido_decimos_percentual = 0;
        estado.estado_operacao = estado.parada_solicitada
            ? CONTROLE_ESTADO_PARADO : CONTROLE_ESTADO_BLOQUEADO;
    } else {
        estado.corte_por_rele_solicitado = 0;
        estado.modo_a_aterrar_solicitado =
            estado.faixa_marcha_solicitada == FAIXA_MARCHA_L;
        estado.modo_b_aterrar_solicitado =
            estado.faixa_marcha_solicitada == FAIXA_MARCHA_H;
        estado.reversao_acionar_solicitado = estado.reversao_permitida &&
            estado.reversao_solicitada;
        const uint16_t permitido = std::min(
            estado.acelerador_lido_decimos_percentual,
            estado.acelerador_maximo_decimos_percentual);
        estado.acelerador_permitido_decimos_percentual = std::min<uint16_t>(
            permitido, static_cast<uint16_t>(
                estado.acelerador_permitido_decimos_percentual +
                configuracao::PASSO_ACELERADOR_DECIMOS));
        estado.estado_operacao = permitido == 0
            ? CONTROLE_ESTADO_PRONTO : CONTROLE_ESTADO_ACELERANDO;
    }
    portEXIT_CRITICAL(&trava);
    if (armado_agora) {
        ESP_LOGI(ETIQUETA, "Partida concluída: controle armado; pedal em zero e chave física validada");
        (void)servico_sinalizacao_notificar(EventoSinalizacao::OperacaoConfirmada);
    }
    if ((bloqueios & ~bloqueios_anteriores) != 0) {
        ESP_LOGW(ETIQUETA, "Controle bloqueado: bits=0x%02x, velocidade=%.2f km/h, aceleração=%.3f m/s2",
                 bloqueios, odometro.velocidade_kmh,
                 aceleracao.aceleracao_longitudinal_ms2);
    }
}

void tarefa(void*) {
    while (true) {
        const uint32_t agora = agora_ms();
        if (!mcp4725_disponivel() &&
            (ultima_sondagem_dac_ms == 0 ||
             agora - ultima_sondagem_dac_ms >= configuracao::INTERVALO_RESONDAGEM_DAC_MS)) {
            ultima_sondagem_dac_ms = agora;
            const esp_err_t erro_dac = mcp4725_iniciar();
            if (erro_dac == ESP_OK) ultima_saida_dac_mv = 0;
            else ESP_LOGW(ETIQUETA, "MCP4725 indisponível: %s",
                          esp_err_to_name(erro_dac));
        }

        uint16_t tensao_pedal_mv = 0;
        const esp_err_t erro_pedal = servico_aquisicao_sensor_ativo(
            SensorControlado::Adc)
            ? leitor_pedal_ler_mv(&tensao_pedal_mv) : ESP_ERR_INVALID_STATE;
        const bool pedal_plausivel = erro_pedal == ESP_OK &&
            tensao_pedal_mv >= configuracao::PEDAL_TENSAO_SOLTO_MV -
                                    configuracao::PEDAL_MARGEM_FALHA_MV &&
            tensao_pedal_mv <= configuracao::PEDAL_TENSAO_PRESSIONADO_MV +
                                    configuracao::PEDAL_MARGEM_FALHA_MV;
        if (!pedal_plausivel) {
            informar_falha_pedal(agora, erro_pedal == ESP_OK
                ? ESP_ERR_INVALID_RESPONSE : erro_pedal);
        } else {
            falhas_pedal = 0;
            const uint32_t curso_mv = configuracao::PEDAL_TENSAO_PRESSIONADO_MV -
                configuracao::PEDAL_TENSAO_SOLTO_MV;
            const uint32_t deslocamento_mv = tensao_pedal_mv >
                    configuracao::PEDAL_TENSAO_SOLTO_MV
                ? tensao_pedal_mv - configuracao::PEDAL_TENSAO_SOLTO_MV : 0;
            portENTER_CRITICAL(&trava);
            const uint32_t zona_mv = curso_mv *
                estado.zona_morta_decimos_percentual / 1000u;
            const uint16_t pedal = deslocamento_mv <= zona_mv ? 0 :
                static_cast<uint16_t>(std::min<uint32_t>(1000,
                    (deslocamento_mv - zona_mv) * 1000u /
                    (curso_mv - zona_mv)));
            estado.tensao_pedal_mv = tensao_pedal_mv;
            estado.pedal_disponivel = 1;
            portEXIT_CRITICAL(&trava);
            (void)servico_controle_remoto_informar_pedal(pedal);
        }
        const auto aceleracao = servico_aceleracao_obter_estado();
        const auto odometro = servico_odometro_obter_estado();
        const bool simulado = servico_simulador_telemetria_esta_ativo();
        uint8_t marcha_lida = FAIXA_MARCHA_INDEFINIDA;
        (void)chave_marcha_ler(&marcha_lida);
        atualizar_logica(agora, aceleracao, odometro, simulado, marcha_lida);

        const auto controle = servico_controle_remoto_obter_estado();
        const bool liberar_dac = configuracao::HABILITAR_SAIDA_ACELERADOR_DAC &&
            mcp4725_disponivel() && !controle.parada_solicitada &&
            !controle.corte_por_rele_solicitado && controle.bloqueios == 0 &&
            controle.pedal_disponivel;
        const uint16_t saida_mv = liberar_dac &&
                controle.acelerador_permitido_decimos_percentual != 0
            ? static_cast<uint16_t>(controle.tensao_minima_saida_mv +
                (configuracao::MCP4725_ALIMENTACAO_MV -
                 controle.tensao_minima_saida_mv) *
                controle.acelerador_permitido_decimos_percentual / 1000u)
            : 0;
        const bool renovar_dac = configuracao::HABILITAR_SAIDA_ACELERADOR_DAC &&
            agora - ultima_escrita_dac_ms >= 100;
        if (mcp4725_disponivel() &&
            (saida_mv != ultima_saida_dac_mv || renovar_dac)) {
            const esp_err_t erro_dac = mcp4725_definir_milivolts(saida_mv);
            if (erro_dac == ESP_OK) {
                ultima_saida_dac_mv = saida_mv;
                ultima_escrita_dac_ms = agora;
            }
            else {
                portENTER_CRITICAL(&trava);
                solicitar_parada(agora);
                portEXIT_CRITICAL(&trava);
                ultima_saida_dac_mv = UINT16_MAX;
                ultima_sondagem_dac_ms = agora;
                ESP_LOGE(ETIQUETA, "Falha na saída do acelerador: %s",
                         esp_err_to_name(erro_dac));
            }
        }
        portENTER_CRITICAL(&trava);
        estado.dac_disponivel = mcp4725_disponivel();
        estado.dac_zero_confirmado = mcp4725_disponivel() &&
            ultima_saida_dac_mv == 0;
        estado.tensao_dac_mv = ultima_saida_dac_mv == UINT16_MAX
            ? 0 : ultima_saida_dac_mv;
        estado.atuacao_habilitada =
            configuracao::HABILITAR_SAIDA_ACELERADOR_DAC &&
            configuracao::HABILITAR_SAIDAS_VEICULO;
        portEXIT_CRITICAL(&trava);
        const esp_err_t erro_saidas = saidas_veiculo_aplicar(
            servico_controle_remoto_obter_estado());
        if (erro_saidas != ESP_OK) {
            portENTER_CRITICAL(&trava);
            falha_saidas = true;
            solicitar_parada(agora);
            portEXIT_CRITICAL(&trava);
            saidas_veiculo_bloquear_falha();
            ESP_LOGE(ETIQUETA, "Falha nas saídas do veículo: %s",
                     esp_err_to_name(erro_saidas));
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONTROLE_REMOTO_PERIODO_MS));
    }
}
} // namespace

esp_err_t servico_controle_remoto_iniciar() {
    const esp_err_t erro_chave = chave_marcha_iniciar();
    if (erro_chave != ESP_OK) return erro_chave;
    const esp_err_t erro_saidas = saidas_veiculo_iniciar();
    if (erro_saidas != ESP_OK) return erro_saidas;
    carregar_limites();
    portENTER_CRITICAL(&trava);
    if (iniciado) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    // Após um reboot, preserve uma janela mínima desarmada desde a partida.
    rearme_disponivel_em_us = std::max<int64_t>(
        rearme_disponivel_em_us, CONTROLE_REMOTO_REARME_MINIMO_MS * 1000LL);
    iniciado = true;
    portEXIT_CRITICAL(&trava);
    if (configuracao::criar_tarefa(tarefa, "controle_remoto", nullptr,
                                   &tarefa_controle) != pdPASS) {
        portENTER_CRITICAL(&trava);
        iniciado = false;
        portEXIT_CRITICAL(&trava);
        ESP_LOGE(ETIQUETA, "Falha ao iniciar tarefa prioritária de controle");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(ETIQUETA, "Pedal ADC no GPIO3 e DAC configurados; saída DAC bloqueada até validação elétrica; GPIOs de marcha e relé indefinidos");
    return ESP_OK;
}

void servico_controle_remoto_concluir_partida() {
    portENTER_CRITICAL(&trava);
    if (iniciado) {
        estado.partida_concluida = 1;
        ++estado.revisao;
        estado.alterado_em_ms = agora_ms();
    }
    portEXIT_CRITICAL(&trava);
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
}

esp_err_t servico_controle_remoto_preparar_ota() {
    portENTER_CRITICAL(&trava);
    if (!iniciado || estado.ota_em_andamento) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    rearme_somente_manual = true; // Permanece após a OTA até o operador rearmar.
    solicitar_parada(agora_ms());
    estado.ota_em_andamento = 1;
    estado.dac_zero_confirmado = 0;
    portEXIT_CRITICAL(&trava);
    const esp_err_t erro_saidas = saidas_veiculo_bloquear_ota();
    if (erro_saidas != ESP_OK) return erro_saidas;
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
    if (!configuracao::HABILITAR_SAIDA_ACELERADOR_DAC) return ESP_OK;
    // Se a atuação vier a ser habilitada, não iniciar a transferência antes
    // de confirmar que o comando de zero chegou ao DAC.
    for (unsigned tentativa = 0; tentativa < 20; ++tentativa) {
        if (servico_controle_remoto_obter_estado().dac_zero_confirmado)
            return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}

void servico_controle_remoto_encerrar_ota() {
    portENTER_CRITICAL(&trava);
    estado.ota_em_andamento = 0;
    // A parada permanece ativa até o operador rearmar.
    portEXIT_CRITICAL(&trava);
    saidas_veiculo_encerrar_ota();
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
}

esp_err_t servico_controle_remoto_definir(AjusteControleRemoto ajuste,
                                           uint32_t valor) {
    switch (ajuste) {
        case AjusteControleRemoto::LimiteMarcha:
            if (valor < FAIXA_MARCHA_L || valor > FAIXA_MARCHA_H)
                return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::VelocidadeMaxima:
            if (valor > CONTROLE_REMOTO_VELOCIDADE_MAXIMA)
                return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::AceleradorMaximo:
            if (valor > CONTROLE_REMOTO_ACELERADOR_MAXIMO)
                return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::ZonaMortaPedal:
            if (valor > CONTROLE_REMOTO_ZONA_MORTA_MAXIMA)
                return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::TensaoMinimaDac:
            if (valor > CONTROLE_REMOTO_TENSAO_MINIMA_MAXIMA_MV)
                return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::Parada:
            if (valor > 1) return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::Reversao:
        case AjusteControleRemoto::PermissaoReversao:
            if (valor > 1) return ESP_ERR_INVALID_ARG;
            break;
        default: return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&trava);
    const int64_t agora_us = esp_timer_get_time();
    const uint32_t agora = static_cast<uint32_t>(agora_us / 1000);
    bool parar_ao_bloquear_re = false;
    if (!iniciado && ajuste != AjusteControleRemoto::Parada) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    if (ajuste == AjusteControleRemoto::Parada) {
        if (valor != 0) solicitar_parada(agora);
        else if (agora_us < rearme_disponivel_em_us ||
                 !estado.partida_concluida || estado.ota_em_andamento ||
                 falha_saidas || !estado.pedal_disponivel ||
                 exige_pedal_zero || pedal_zero_desde_ms == 0 ||
                 agora - pedal_zero_desde_ms < configuracao::PEDAL_SOLTO_PARA_ARMAR_MS ||
                 pedal_recebido_em_ms == 0 ||
                 agora - pedal_recebido_em_ms > CONTROLE_REMOTO_EXPIRACAO_PEDAL_MS ||
                 estado.acelerador_lido_decimos_percentual != 0 ||
                 estado.faixa_marcha_fisica == FAIXA_MARCHA_INDEFINIDA ||
                 estado.velocidade_medida_centesimos_kmh >
                     configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS ||
                 estado.velocidade_maxima_centesimos_kmh <=
                     estado.velocidade_medida_centesimos_kmh ||
                 estado.acelerador_maximo_decimos_percentual == 0 ||
                 (estado.bloqueios & (CONTROLE_BLOQUEIO_SEM_SENSORES |
                                      CONTROLE_BLOQUEIO_TELEMETRIA_SIMULADA)) != 0 ||
                 (configuracao::HABILITAR_SAIDA_ACELERADOR_DAC &&
                  !estado.dac_zero_confirmado)) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        } else liberar_parada(agora);
    } else if (ajuste == AjusteControleRemoto::AceleradorMaximo) {
        if (valor > estado.acelerador_maximo_decimos_percentual &&
            estado.acelerador_lido_decimos_percentual != 0) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        }
        estado.acelerador_maximo_decimos_percentual = static_cast<uint16_t>(valor);
        if (estado.acelerador_permitido_decimos_percentual > valor)
            estado.acelerador_permitido_decimos_percentual = static_cast<uint16_t>(valor);
        if (valor == 0) {
            cancelar_acelerador();
            estado.corte_por_rele_solicitado = 1;
        }
    } else if (ajuste == AjusteControleRemoto::ZonaMortaPedal ||
               ajuste == AjusteControleRemoto::TensaoMinimaDac) {
        if (estado.acelerador_lido_decimos_percentual != 0 ||
            estado.acelerador_permitido_decimos_percentual != 0) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        }
        if (ajuste == AjusteControleRemoto::ZonaMortaPedal)
            estado.zona_morta_decimos_percentual = static_cast<uint16_t>(valor);
        else estado.tensao_minima_saida_mv = static_cast<uint16_t>(valor);
        cancelar_acelerador();
    } else if (ajuste == AjusteControleRemoto::PermissaoReversao) {
        estado.reversao_permitida = static_cast<uint8_t>(valor);
        parar_ao_bloquear_re = valor == 0 &&
            (estado.reversao_solicitada || estado.reversao_acionar_solicitado);
        if (parar_ao_bloquear_re) solicitar_parada(agora);
    } else if (ajuste == AjusteControleRemoto::Reversao) {
        if ((valor != 0 && !estado.reversao_permitida) || estado.parada_solicitada ||
            estado.acelerador_lido_decimos_percentual != 0 ||
            estado.acelerador_permitido_decimos_percentual != 0 ||
            (estado.bloqueios & CONTROLE_BLOQUEIO_SEM_SENSORES) != 0 ||
            estado.velocidade_medida_centesimos_kmh >
                configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        }
        estado.reversao_solicitada = static_cast<uint8_t>(valor);
        cancelar_acelerador();
        estado.corte_por_rele_solicitado = 1;
    } else if (ajuste == AjusteControleRemoto::LimiteMarcha) {
        const uint8_t efetiva = estado.faixa_marcha_fisica == FAIXA_MARCHA_INDEFINIDA
            ? static_cast<uint8_t>(FAIXA_MARCHA_INDEFINIDA)
            : std::min<uint8_t>(estado.faixa_marcha_fisica,
                                static_cast<uint8_t>(valor));
        if (efetiva != estado.faixa_marcha_solicitada &&
            (estado.acelerador_lido_decimos_percentual != 0 ||
             estado.velocidade_medida_centesimos_kmh >
                 configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS)) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        }
        estado.faixa_marcha_maxima = static_cast<uint8_t>(valor);
        if (estado.faixa_marcha_solicitada != efetiva) {
            estado.faixa_marcha_solicitada = efetiva;
            ultima_marcha_confirmada = efetiva;
            cancelar_acelerador();
            estado.corte_por_rele_solicitado = 1;
            estado.modo_a_aterrar_solicitado = 0;
            estado.modo_b_aterrar_solicitado = 0;
            estado.reversao_acionar_solicitado = 0;
        }
    } else if (ajuste == AjusteControleRemoto::VelocidadeMaxima) {
        estado.velocidade_maxima_centesimos_kmh = valor;
        cancelar_acelerador();
        estado.corte_por_rele_solicitado = 1;
    }
    ++estado.revisao;
    estado.alterado_em_ms = agora;
    const uint32_t revisao = estado.revisao;
    portEXIT_CRITICAL(&trava);
    if (ajuste == AjusteControleRemoto::PermissaoReversao) {
        const esp_err_t erro_saida = saidas_veiculo_definir_permissao_reversao(
            valor != 0, parar_ao_bloquear_re, revisao);
        if (erro_saida != ESP_OK) {
            portENTER_CRITICAL(&trava);
            estado.reversao_permitida = 0;
            falha_saidas = true;
            solicitar_parada(agora_ms());
            portEXIT_CRITICAL(&trava);
            saidas_veiculo_bloquear_falha();
            if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
            return erro_saida;
        }
    }
    if (ajuste == AjusteControleRemoto::VelocidadeMaxima ||
        ajuste == AjusteControleRemoto::AceleradorMaximo) {
        const esp_err_t erro_nvs = salvar_limite(
            ajuste == AjusteControleRemoto::VelocidadeMaxima
                ? CHAVE_VELOCIDADE : CHAVE_ACELERADOR, valor);
        if (erro_nvs != ESP_OK) {
            // Não deixar o limite alterado apenas em RAM após falha ao salvar.
            portENTER_CRITICAL(&trava);
            if (ajuste == AjusteControleRemoto::VelocidadeMaxima)
                estado.velocidade_maxima_centesimos_kmh = 0;
            else estado.acelerador_maximo_decimos_percentual = 0;
            solicitar_parada(agora_ms());
            portEXIT_CRITICAL(&trava);
            ESP_LOGE(ETIQUETA, "Falha ao salvar limite: %s",
                     esp_err_to_name(erro_nvs));
            return erro_nvs;
        }
    }
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
    ESP_LOGI(ETIQUETA, "Controle: ajuste=%u valor=%lu revisão=%lu",
             static_cast<unsigned>(ajuste), static_cast<unsigned long>(valor),
             static_cast<unsigned long>(revisao));
    return ESP_OK;
}

estado_controle_remoto_t servico_controle_remoto_obter_estado() {
    portENTER_CRITICAL(&trava);
    auto copia = estado;
    const int64_t agora_us = esp_timer_get_time();
    const uint32_t agora = static_cast<uint32_t>(agora_us / 1000);
    copia.parada_restante_ms = restante_ms(parada_expira_em_us, agora_us);
    copia.rearme_restante_ms = restante_ms(rearme_disponivel_em_us, agora_us);
    copia.amostra_pedal_restante_ms = pedal_recebido_em_ms != 0 &&
        agora - pedal_recebido_em_ms < CONTROLE_REMOTO_EXPIRACAO_PEDAL_MS
        ? CONTROLE_REMOTO_EXPIRACAO_PEDAL_MS - (agora - pedal_recebido_em_ms) : 0;
    portEXIT_CRITICAL(&trava);
    return copia;
}

esp_err_t servico_controle_remoto_informar_pedal(uint16_t decimos_percentual) {
    if (decimos_percentual > CONTROLE_REMOTO_ACELERADOR_MAXIMO)
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&trava);
    if (!iniciado) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    estado.acelerador_lido_decimos_percentual = decimos_percentual;
    pedal_recebido_em_ms = agora_ms();
    if (decimos_percentual == 0) {
        if (pedal_zero_desde_ms == 0) pedal_zero_desde_ms = pedal_recebido_em_ms;
        exige_pedal_zero = false;
    } else pedal_zero_desde_ms = 0;
    portEXIT_CRITICAL(&trava);
    return ESP_OK;
}
