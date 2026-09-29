#include "servicos/servico_comandos.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "servicos/servico_diagnostico.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_telemetria.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "servico_comandos";
constexpr unsigned MAXIMO_TENTATIVAS = 3;
constexpr uint32_t CAPACIDADES_MESTRE =
    CAPACIDADE_COMANDO_PING | CAPACIDADE_COMANDO_VERSAO |
    CAPACIDADE_COMANDO_SAUDE | CAPACIDADE_COMANDO_TELEMETRIA |
    CAPACIDADE_COMANDO_UART | CAPACIDADE_COMANDO_OTA |
    CAPACIDADE_COMANDO_WIFI | CAPACIDADE_COMANDO_CONTROLE_OTA |
    CAPACIDADE_COMANDO_TERMINAL;

struct RespostaPendente {
    DestinoUart origem;
    cabecalho_quadro_comando_t cabecalho;
    uint8_t carga[COMANDO_TAMANHO_MAXIMO_CARGA];
};

struct RespostaRepetivel {
    uint32_t solicitacao = 0;
    uint16_t comando = 0;
    size_t tamanho = 0;
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO]{};
};

using FuncaoComando = codigo_resposta_comando_t (*)(
    DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta);

struct RegistroComando {
    codigo_comando_t codigo;
    FuncaoComando executar;
};

QueueHandle_t fila_respostas = nullptr;
SemaphoreHandle_t mutex_solicitacao = nullptr;
portMUX_TYPE trava_estatisticas = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE trava_repeticoes = portMUX_INITIALIZER_UNLOCKED;
EstatisticasServicoComandos estatisticas{};
RespostaRepetivel respostas_repetiveis[2]{};
AcaoSolicitarResumo acao_solicitar_resumo = nullptr;

uint8_t no_para_destino(DestinoUart destino) {
    return destino == DestinoUart::Equipe ? NO_COMANDO_EQUIPE
                                          : NO_COMANDO_VISITANTES;
}

size_t indice_destino(DestinoUart destino) {
    return destino == DestinoUart::Equipe ? 0u : 1u;
}

void registrar_erro(esp_err_t erro) {
    portENTER_CRITICAL(&trava_estatisticas);
    estatisticas.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_estatisticas);
}

void copiar_texto(char* destino, size_t capacidade, const char* origem) {
    if (destino == nullptr || capacidade == 0) return;
    const char* texto = origem == nullptr ? "" : origem;
    const size_t tamanho = std::min(std::strlen(texto), capacidade - 1);
    std::memcpy(destino, texto, tamanho);
    destino[tamanho] = '\0';
}

codigo_resposta_comando_t validar_sem_carga(uint16_t tamanho_carga) {
    return tamanho_carga == 0 ? RESPOSTA_COMANDO_OK
                              : RESPOSTA_COMANDO_CARGA_INVALIDA;
}

codigo_resposta_comando_t responder_ping(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const resposta_comando_ping_t dados = {
        static_cast<uint32_t>(esp_timer_get_time() / 1000)};
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_versao(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    resposta_comando_versao_t dados{};
    copiar_texto(dados.versao, sizeof(dados.versao),
                 esp_app_get_description()->version);
    dados.capacidades = CAPACIDADES_MESTRE;
    dados.no = NO_COMANDO_MESTRE;
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_capacidades(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const resposta_comando_capacidades_t dados = {CAPACIDADES_MESTRE};
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_saude(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const ResumoSaudeSistema saude = servico_diagnostico_obter_resumo();
    const resposta_comando_saude_t dados = {
        static_cast<uint8_t>(saude.estado), saude.causas_ativas,
        static_cast<uint32_t>(saude.tempo_ativo_ms), saude.heap_livre,
        saude.erros_sensores + saude.erros_i2c + saude.erros_uart +
            saude.falhas_ota + saude.falhas_supervisao};
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

uint16_t escalar_uint16(float valor, float fator) {
    return static_cast<uint16_t>(std::lround(
        std::clamp(valor * fator, 0.0f, 65535.0f)));
}

codigo_resposta_comando_t responder_telemetria(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const DadosTelemetriaVeiculo atual = servico_telemetria_obter_ultimos_dados();
    const EstatisticasTelemetria estat = servico_telemetria_obter_estatisticas();
    resposta_comando_telemetria_t dados{};
    dados.sequencia = estat.ultima_sequencia;
    dados.tempo_mestre_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    dados.tensao_adc_bruta = atual.tensao_adc_bruta;
    dados.aceleracao_x = atual.aceleracao_x;
    dados.aceleracao_y = atual.aceleracao_y;
    dados.aceleracao_z = atual.aceleracao_z;
    dados.giroscopio_x = atual.giroscopio_x;
    dados.giroscopio_y = atual.giroscopio_y;
    dados.giroscopio_z = atual.giroscopio_z;
    dados.velocidade_centesimos_kmh = escalar_uint16(atual.velocidade_kmh, 100.0f);
    dados.carga_decimos_percentual = escalar_uint16(atual.carga_percentual, 10.0f);
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_uart(
    DestinoUart origem, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const EstatisticasUart uart = gerenciador_uart_obter_estatisticas(origem);
    const resposta_comando_uart_t dados = {
        uart.envios_ok, uart.recepcoes_ok,
        uart.erros_envio + uart.erros_recepcao,
        uart.mensagens_descartadas};
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_ota(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const SituacaoOta ota = servico_ota_obter_situacao();
    resposta_comando_ota_detalhada_t dados{};
    dados.versao_formato = COMANDO_FORMATO_OTA_DETALHADO;
    dados.estado = static_cast<uint8_t>(ota.estado);
    dados.alvo_ativo = static_cast<uint8_t>(ota.alvo_ativo);
    dados.estado_equipe = static_cast<uint8_t>(ota.equipe.estado);
    dados.estado_visitantes = static_cast<uint8_t>(ota.visitantes.estado);
    EstadoServicoOta estado_mestre = ota.estado;
    if (ota.alvo_ativo != AlvoOta::Mestre) {
        switch (ota.estado) {
            case EstadoServicoOta::Desabilitado:
            case EstadoServicoOta::Inicializando:
            case EstadoServicoOta::AguardandoRede:
            case EstadoServicoOta::Verificando:
            case EstadoServicoOta::Cancelado:
                break;
            default:
                estado_mestre = ota.versao_disponivel[0] != '\0'
                                     ? EstadoServicoOta::AguardandoAutorizacao
                                     : EstadoServicoOta::Atualizado;
                break;
        }
    }
    dados.estado_mestre = static_cast<uint8_t>(estado_mestre);
    dados.progresso_mestre_decimos = static_cast<uint16_t>(
        std::min<uint32_t>(ota.progresso.percentual_decimos, 1000u));
    dados.progresso_equipe_decimos = static_cast<uint16_t>(
        std::min<uint32_t>(ota.equipe.progresso.percentual_decimos, 1000u));
    dados.progresso_visitantes_decimos = static_cast<uint16_t>(
        std::min<uint32_t>(ota.visitantes.progresso.percentual_decimos, 1000u));
    dados.falhas = ota.falhas;
    dados.verificacoes = ota.verificacoes;
    copiar_texto(dados.versao_mestre_atual, sizeof(dados.versao_mestre_atual),
                 ota.versao_atual);
    copiar_texto(dados.versao_mestre_disponivel,
                 sizeof(dados.versao_mestre_disponivel),
                 ota.versao_disponivel);
    copiar_texto(dados.versao_equipe_atual, sizeof(dados.versao_equipe_atual),
                 ota.equipe.versao_atual);
    copiar_texto(dados.versao_equipe_disponivel,
                 sizeof(dados.versao_equipe_disponivel),
                 ota.equipe.versao_disponivel);
    copiar_texto(dados.versao_visitantes_atual,
                 sizeof(dados.versao_visitantes_atual),
                 ota.visitantes.versao_atual);
    copiar_texto(dados.versao_visitantes_disponivel,
                 sizeof(dados.versao_visitantes_disponivel),
                 ota.visitantes.versao_disponivel);
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_wifi(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    resposta_comando_wifi_t dados{};
    dados.radio_ativo = wifi.radio_ativo;
    dados.conectado = wifi.conectado;
    dados.rssi_dbm = wifi.rssi_dbm;
    dados.canal = wifi.canal;
    copiar_texto(dados.rede, sizeof(dados.rede), wifi.rede_ativa);
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t converter_resultado_acao(esp_err_t erro) {
    if (erro == ESP_OK) return RESPOSTA_COMANDO_OK;
    if (erro == ESP_ERR_INVALID_ARG) return RESPOSTA_COMANDO_CARGA_INVALIDA;
    if (erro == ESP_ERR_INVALID_STATE || erro == ESP_ERR_TIMEOUT)
        return RESPOSTA_COMANDO_OCUPADO;
    return RESPOSTA_COMANDO_ERRO_INTERNO;
}

codigo_resposta_comando_t executar_acao_ota(
    DestinoUart origem, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta, codigo_comando_t comando) {
    *tamanho_resposta = 0;
    if (origem != DestinoUart::Equipe) {
        ESP_LOGW(ETIQUETA,
                 "Ação remota %u negada: origem sem permissão",
                 static_cast<unsigned>(comando));
        return RESPOSTA_COMANDO_NEGADO;
    }
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    esp_err_t erro = ESP_ERR_NOT_SUPPORTED;
    switch (comando) {
        case COMANDO_SOLICITAR_VERIFICACAO_OTA:
            erro = servico_ota_solicitar_verificacao();
            break;
        case COMANDO_AUTORIZAR_OTA_MESTRE:
            erro = servico_ota_autorizar_atualizacao();
            break;
        case COMANDO_AUTORIZAR_OTA_EQUIPE:
            erro = servico_ota_autorizar_atualizacao_satelite(AlvoOta::Equipe);
            break;
        case COMANDO_AUTORIZAR_OTA_VISITANTES:
            erro = servico_ota_autorizar_atualizacao_satelite(AlvoOta::Visitantes);
            break;
        case COMANDO_CANCELAR_OTA:
            erro = servico_ota_cancelar_atualizacao();
            break;
        default:
            break;
    }
    const codigo_resposta_comando_t resultado = converter_resultado_acao(erro);
    if (resultado == RESPOSTA_COMANDO_OK) {
        const resposta_comando_acao_t confirmacao = {
            static_cast<uint16_t>(comando), 1, 0,
            static_cast<uint32_t>(esp_timer_get_time() / 1000)};
        std::memcpy(resposta, &confirmacao, sizeof(confirmacao));
        *tamanho_resposta = sizeof(confirmacao);
        ESP_LOGI(ETIQUETA, "Ação remota %u aceita para processamento",
                 static_cast<unsigned>(comando));
    } else {
        ESP_LOGW(ETIQUETA, "Ação remota %u recusada: %s",
                 static_cast<unsigned>(comando), esp_err_to_name(erro));
    }
    return resultado;
}

#define DEFINIR_ACAO_OTA(nome, codigo)                                      \
    codigo_resposta_comando_t nome(                                         \
        DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,    \
        uint8_t* resposta, uint16_t* tamanho_resposta) {                     \
        return executar_acao_ota(origem, carga, tamanho_carga, resposta,     \
                                 tamanho_resposta, codigo);                   \
    }

DEFINIR_ACAO_OTA(solicitar_verificacao_ota, COMANDO_SOLICITAR_VERIFICACAO_OTA)
DEFINIR_ACAO_OTA(autorizar_ota_mestre, COMANDO_AUTORIZAR_OTA_MESTRE)
DEFINIR_ACAO_OTA(autorizar_ota_equipe, COMANDO_AUTORIZAR_OTA_EQUIPE)
DEFINIR_ACAO_OTA(autorizar_ota_visitantes, COMANDO_AUTORIZAR_OTA_VISITANTES)
DEFINIR_ACAO_OTA(cancelar_ota, COMANDO_CANCELAR_OTA)

#undef DEFINIR_ACAO_OTA

codigo_resposta_comando_t solicitar_resumo_terminal(
    DestinoUart origem, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    *tamanho_resposta = 0;
    if (origem != DestinoUart::Equipe) {
        ESP_LOGW(ETIQUETA,
                 "Solicitação remota de resumo negada: origem sem permissão");
        return RESPOSTA_COMANDO_NEGADO;
    }
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    if (acao_solicitar_resumo == nullptr) {
        ESP_LOGE(ETIQUETA, "Ação de resumo não foi configurada pela aplicação");
        return RESPOSTA_COMANDO_ERRO_INTERNO;
    }
    acao_solicitar_resumo();
    const resposta_comando_acao_t confirmacao = {
        static_cast<uint16_t>(COMANDO_SOLICITAR_RESUMO_TERMINAL), 1, 0,
        static_cast<uint32_t>(esp_timer_get_time() / 1000)};
    std::memcpy(resposta, &confirmacao, sizeof(confirmacao));
    *tamanho_resposta = sizeof(confirmacao);
    return RESPOSTA_COMANDO_OK;
}

constexpr RegistroComando COMANDOS[] = {
    {COMANDO_PING, responder_ping},
    {COMANDO_OBTER_VERSAO, responder_versao},
    {COMANDO_OBTER_CAPACIDADES, responder_capacidades},
    {COMANDO_OBTER_SAUDE, responder_saude},
    {COMANDO_OBTER_TELEMETRIA, responder_telemetria},
    {COMANDO_OBTER_ESTADO_UART, responder_uart},
    {COMANDO_OBTER_ESTADO_OTA, responder_ota},
    {COMANDO_OBTER_ESTADO_WIFI, responder_wifi},
    {COMANDO_SOLICITAR_VERIFICACAO_OTA, solicitar_verificacao_ota},
    {COMANDO_AUTORIZAR_OTA_MESTRE, autorizar_ota_mestre},
    {COMANDO_AUTORIZAR_OTA_EQUIPE, autorizar_ota_equipe},
    {COMANDO_AUTORIZAR_OTA_VISITANTES, autorizar_ota_visitantes},
    {COMANDO_CANCELAR_OTA, cancelar_ota},
    {COMANDO_SOLICITAR_RESUMO_TERMINAL, solicitar_resumo_terminal},
};

const RegistroComando* localizar_comando(uint16_t codigo) {
    for (const auto& comando : COMANDOS) {
        if (comando.codigo == codigo) return &comando;
    }
    return nullptr;
}

void processar_solicitacao(DestinoUart origem,
                           const cabecalho_quadro_comando_t& cabecalho,
                           const uint8_t* carga) {
    ESP_LOGI(ETIQUETA, "Comando recebido de %s: código=%u solicitação=%lu",
             origem == DestinoUart::Equipe ? "equipe" : "visitantes",
             static_cast<unsigned>(cabecalho.comando),
             static_cast<unsigned long>(cabecalho.solicitacao));
    const size_t indice = indice_destino(origem);
    portENTER_CRITICAL(&trava_repeticoes);
    const bool repetida = respostas_repetiveis[indice].solicitacao ==
                              cabecalho.solicitacao &&
                          respostas_repetiveis[indice].comando ==
                              cabecalho.comando;
    const size_t tamanho_repetido = respostas_repetiveis[indice].tamanho;
    uint8_t quadro_repetido[COMANDO_TAMANHO_MAXIMO_QUADRO];
    if (repetida && tamanho_repetido > 0)
        std::memcpy(quadro_repetido, respostas_repetiveis[indice].quadro,
                    tamanho_repetido);
    portEXIT_CRITICAL(&trava_repeticoes);
    if (repetida && tamanho_repetido > 0) {
        const esp_err_t erro =
            gerenciador_uart_enviar(origem, quadro_repetido, tamanho_repetido);
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.repeticoes++;
        if (erro == ESP_OK) {
            estatisticas.respostas_enviadas++;
            estatisticas.ultimo_erro = ESP_OK;
        } else {
            estatisticas.ultimo_erro = erro;
        }
        portEXIT_CRITICAL(&trava_estatisticas);
        return;
    }

    uint8_t carga_resposta[COMANDO_TAMANHO_MAXIMO_CARGA]{};
    uint16_t tamanho_resposta = 0;
    const RegistroComando* registro = localizar_comando(cabecalho.comando);
    codigo_resposta_comando_t resultado = RESPOSTA_COMANDO_NAO_SUPORTADO;
    if (registro != nullptr) {
        resultado = registro->executar(origem, carga, cabecalho.tamanho_carga,
                                       carga_resposta, &tamanho_resposta);
    }
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_comandos_montar_quadro(
        quadro, sizeof(quadro), TIPO_QUADRO_COMANDO_RESPOSTA,
        NO_COMANDO_MESTRE, cabecalho.origem, cabecalho.comando,
        cabecalho.solicitacao, resultado, carga_resposta, tamanho_resposta);
    if (tamanho != 0) {
        // A resposta é memorizada antes do envio: se o ACK se perder, a
        // retransmissão não executará novamente uma ação com efeito colateral.
        portENTER_CRITICAL(&trava_repeticoes);
        respostas_repetiveis[indice].solicitacao = cabecalho.solicitacao;
        respostas_repetiveis[indice].comando = cabecalho.comando;
        respostas_repetiveis[indice].tamanho = tamanho;
        std::memcpy(respostas_repetiveis[indice].quadro, quadro, tamanho);
        portEXIT_CRITICAL(&trava_repeticoes);
    }
    const esp_err_t erro = tamanho == 0 ? ESP_ERR_INVALID_SIZE
        : gerenciador_uart_enviar(origem, quadro, tamanho);
    portENTER_CRITICAL(&trava_estatisticas);
    estatisticas.solicitacoes_recebidas++;
    if (erro == ESP_OK) {
        estatisticas.respostas_enviadas++;
        estatisticas.ultimo_erro = ESP_OK;
    } else {
        estatisticas.ultimo_erro = erro;
    }
    portEXIT_CRITICAL(&trava_estatisticas);
    ESP_LOGI(ETIQUETA,
             "Resposta ao comando %u enviada para %s: resultado=%u transporte=%s",
             static_cast<unsigned>(cabecalho.comando),
             origem == DestinoUart::Equipe ? "equipe" : "visitantes",
             static_cast<unsigned>(resultado), esp_err_to_name(erro));
}
}  // namespace

void servico_comandos_definir_acao_resumo(AcaoSolicitarResumo acao) {
    acao_solicitar_resumo = acao;
}

esp_err_t servico_comandos_iniciar() {
    if (fila_respostas != nullptr && mutex_solicitacao != nullptr) return ESP_OK;
    fila_respostas = xQueueCreate(8, sizeof(RespostaPendente));
    mutex_solicitacao = xSemaphoreCreateMutex();
    if (fila_respostas == nullptr || mutex_solicitacao == nullptr) {
        if (fila_respostas != nullptr) vQueueDelete(fila_respostas);
        if (mutex_solicitacao != nullptr) vSemaphoreDelete(mutex_solicitacao);
        fila_respostas = nullptr;
        mutex_solicitacao = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA,
             "Canal bidirecional pronto: consultas gerais e ações autorizadas da equipe");
    return ESP_OK;
}

void servico_comandos_processar_quadro(DestinoUart origem,
                                       const uint8_t* quadro, size_t tamanho) {
    if (quadro == nullptr || !protocolo_comandos_validar_quadro(quadro, tamanho)) {
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.quadros_invalidos++;
        portEXIT_CRITICAL(&trava_estatisticas);
        return;
    }
    cabecalho_quadro_comando_t cabecalho{};
    std::memcpy(&cabecalho, quadro, sizeof(cabecalho));
    const uint8_t origem_esperada = no_para_destino(origem);
    if (cabecalho.origem != origem_esperada ||
        cabecalho.destino != NO_COMANDO_MESTRE) {
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.quadros_invalidos++;
        portEXIT_CRITICAL(&trava_estatisticas);
        ESP_LOGW(ETIQUETA, "Comando com identidade incompatível descartado");
        return;
    }
    const uint8_t* carga = quadro + sizeof(cabecalho);
    if (cabecalho.tipo == TIPO_QUADRO_COMANDO_SOLICITACAO) {
        processar_solicitacao(origem, cabecalho, carga);
        return;
    }
    RespostaPendente pendente{};
    pendente.origem = origem;
    pendente.cabecalho = cabecalho;
    if (cabecalho.tamanho_carga != 0)
        std::memcpy(pendente.carga, carga, cabecalho.tamanho_carga);
    if (fila_respostas == nullptr ||
        xQueueSend(fila_respostas, &pendente, 0) != pdTRUE) {
        registrar_erro(ESP_ERR_TIMEOUT);
    } else {
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.respostas_recebidas++;
        portEXIT_CRITICAL(&trava_estatisticas);
    }
}

esp_err_t servico_comandos_solicitar(
    DestinoUart destino, codigo_comando_t comando, const void* carga,
    uint16_t tamanho_carga, void* resposta, size_t capacidade_resposta,
    uint16_t* tamanho_resposta, codigo_resposta_comando_t* resultado,
    TickType_t tempo_limite) {
    if (fila_respostas == nullptr || mutex_solicitacao == nullptr ||
        (destino != DestinoUart::Equipe && destino != DestinoUart::Visitantes) ||
        comando == 0 || tempo_limite == 0 ||
        tamanho_carga > COMANDO_TAMANHO_MAXIMO_CARGA ||
        (tamanho_carga != 0 && carga == nullptr) || tamanho_resposta == nullptr ||
        resultado == nullptr) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(mutex_solicitacao, tempo_limite) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    *tamanho_resposta = 0;
    *resultado = RESPOSTA_COMANDO_TIMEOUT;
    RespostaPendente descartada{};
    while (xQueueReceive(fila_respostas, &descartada, 0) == pdTRUE) {}
    uint32_t solicitacao = esp_random();
    if (solicitacao == 0) solicitacao = 1;
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_comandos_montar_quadro(
        quadro, sizeof(quadro), TIPO_QUADRO_COMANDO_SOLICITACAO,
        NO_COMANDO_MESTRE, no_para_destino(destino), comando, solicitacao,
        RESPOSTA_COMANDO_OK, carga, tamanho_carga);
    if (tamanho == 0) {
        registrar_erro(ESP_ERR_INVALID_SIZE);
        xSemaphoreGive(mutex_solicitacao);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t erro = ESP_ERR_TIMEOUT;
    for (unsigned tentativa = 1; tentativa <= MAXIMO_TENTATIVAS; ++tentativa) {
        erro = gerenciador_uart_enviar(destino, quadro, tamanho, tempo_limite);
        if (erro != ESP_OK) continue;
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.solicitacoes_enviadas++;
        if (tentativa > 1) estatisticas.repeticoes++;
        portEXIT_CRITICAL(&trava_estatisticas);
        RespostaPendente recebida{};
        const TickType_t inicio = xTaskGetTickCount();
        while (true) {
            const TickType_t decorrido = xTaskGetTickCount() - inicio;
            if (decorrido >= tempo_limite) break;
            if (xQueueReceive(fila_respostas, &recebida,
                              tempo_limite - decorrido) != pdTRUE) break;
            if (recebida.origem != destino ||
                recebida.cabecalho.solicitacao != solicitacao ||
                recebida.cabecalho.comando != comando) continue;
            *resultado = static_cast<codigo_resposta_comando_t>(
                recebida.cabecalho.resultado);
            *tamanho_resposta = recebida.cabecalho.tamanho_carga;
            if (*tamanho_resposta > capacidade_resposta ||
                (*tamanho_resposta != 0 && resposta == nullptr)) {
                erro = ESP_ERR_INVALID_SIZE;
            } else {
                if (*tamanho_resposta != 0)
                    std::memcpy(resposta, recebida.carga, *tamanho_resposta);
                erro = ESP_OK;
            }
            xSemaphoreGive(mutex_solicitacao);
            registrar_erro(erro);
            return erro;
        }
    }
    portENTER_CRITICAL(&trava_estatisticas);
    estatisticas.timeouts++;
    estatisticas.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_estatisticas);
    xSemaphoreGive(mutex_solicitacao);
    ESP_LOGW(ETIQUETA, "Sem resposta de %s ao comando %u após %u tentativas",
             destino == DestinoUart::Equipe ? "equipe" : "visitantes",
             static_cast<unsigned>(comando), MAXIMO_TENTATIVAS);
    return erro;
}

EstatisticasServicoComandos servico_comandos_obter_estatisticas() {
    portENTER_CRITICAL(&trava_estatisticas);
    const EstatisticasServicoComandos copia = estatisticas;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}
