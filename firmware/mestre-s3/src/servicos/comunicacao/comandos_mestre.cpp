// Adapta os comandos UART às consultas e ações dos serviços do mestre.
#include "servicos/comunicacao/comandos_uart.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gerenciadores/i2c.h"
#include "gerenciadores/uart.h"
#include "gerenciadores/wifi.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/veiculo.h"
#include "servicos/manutencao/ota.h"
#include "servicos/comunicacao/wifi_sessoes.h"
#include "servicos/veiculo/odometro.h"
#include "servicos/veiculo/simulador.h"
#include "servicos/comunicacao/telemetria_uart.h"
#include "servicos/veiculo/controle.h"
#include "servicos/manutencao/ajustes.h"
#include "servicos/veiculo/aquisicao.h"
#include "servicos/manutencao/diagnostico.h"
#include "servicos/historico/registro_sd.h"
#include "servicos/interface_local/relatorios.h"
#include "servicos/interface_local/terminal/console.h"

namespace {
constexpr char ETIQUETA[] = "servico_comandos";
constexpr uint32_t CAPACIDADES_MESTRE =
    CAPACIDADE_COMANDO_PING | CAPACIDADE_COMANDO_VERSAO |
    CAPACIDADE_COMANDO_SAUDE | CAPACIDADE_COMANDO_TELEMETRIA |
    CAPACIDADE_COMANDO_UART | CAPACIDADE_COMANDO_OTA |
    CAPACIDADE_COMANDO_WIFI | CAPACIDADE_COMANDO_CONTROLE_OTA |
    CAPACIDADE_COMANDO_TERMINAL |
    CAPACIDADE_COMANDO_CONTROLE_OPERACIONAL |
    CAPACIDADE_COMANDO_DIAGNOSTICO |
    CAPACIDADE_COMANDO_SINCRONIZACAO |
    CAPACIDADE_COMANDO_SIMULADOR | CAPACIDADE_COMANDO_CONTROLE_REMOTO |
    CAPACIDADE_COMANDO_SENSORES_MESTRE |
    CAPACIDADE_COMANDO_RESTAURAR_POLITICA_WIFI | CAPACIDADE_COMANDO_LOGS_TERMINAL |
    CAPACIDADE_COMANDO_TAREFAS_MESTRE | CAPACIDADE_COMANDO_PERMISSAO_REVERSAO |
    CAPACIDADE_COMANDO_AJUSTES_DESENVOLVEDOR |
    (configuracao::HABILITAR_DAC_ISOLADO ? CAPACIDADE_COMANDO_DAC_ISOLADO : 0u);

static_assert(static_cast<uint8_t>(SensorControlado::Adc) == SENSOR_MESTRE_ADC &&
              static_cast<uint8_t>(SensorControlado::Mpu6050) == SENSOR_MESTRE_MPU6050 &&
              static_cast<uint8_t>(SensorControlado::Encoders) == SENSOR_MESTRE_ENCODERS &&
              static_cast<uint8_t>(SensorControlado::Ina228) == SENSOR_MESTRE_INA228 &&
              static_cast<uint8_t>(SensorControlado::TemperaturaEsp) == SENSOR_MESTRE_TEMPERATURA_ESP,
              "Os bits de sensores devem coincidir com os do protocolo");
static_assert(QUANTIDADE_FONTES_LOG_TERMINAL == COMANDO_FONTES_LOG_TERMINAL &&
              ESP_LOG_WARN == COMANDO_NIVEL_LOG_AVISOS && ESP_LOG_INFO == COMANDO_NIVEL_LOG_INFORMACOES &&
              static_cast<uint8_t>(AjusteLogsTerminal::Global) == AJUSTE_LOG_TERMINAL_GLOBAL &&
              static_cast<uint8_t>(AjusteLogsTerminal::ApenasAvisosProjeto) == AJUSTE_LOG_TERMINAL_APENAS_PROJETO &&
              static_cast<uint8_t>(AjusteLogsTerminal::Cores) == AJUSTE_LOG_TERMINAL_CORES &&
              static_cast<uint8_t>(AjusteLogsTerminal::Fonte) == AJUSTE_LOG_TERMINAL_FONTE,
              "Os identificadores dos logs devem coincidir com os do protocolo");
static_assert(MAXIMO_TAREFAS_DIAGNOSTICO == COMANDO_MAXIMO_TAREFAS_MESTRE,
              "O limite das tarefas deve coincidir com o do protocolo");

// O painel pode alterar aquisição somente em firmware sem atuação física
// habilitada. As leituras seguem disponíveis em qualquer configuração.
constexpr bool CONTROLE_SENSORES_REMOTO_PERMITIDO =
    !configuracao::HABILITAR_SAIDA_ACELERADOR_DAC && !configuracao::HABILITAR_SAIDAS_VEICULO;

using FuncaoComando = codigo_resposta_comando_t (*)(
    DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta);

struct RegistroComando {
    codigo_comando_t codigo;
    FuncaoComando executar;
};

struct RedeWifiPreparada {
    char ssid[33]{};
    char senha[64]{};
    bool valida = false;
};

RedeWifiPreparada rede_wifi_preparada{};

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

codigo_resposta_comando_t responder_telemetria(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    pacote_telemetria_t atual{};
    if (!servico_telemetria_obter_ultimo_pacote_validado(atual)) {
        return RESPOSTA_COMANDO_OCUPADO;
    }
    resposta_comando_telemetria_t dados{};
    dados.sequencia = atual.sequencia;
    dados.tempo_mestre_ms = atual.tempo_mestre_ms;
    dados.tensao_adc_bruta = atual.tensao_adc_bruta;
    dados.aceleracao_x = atual.aceleracao_x;
    dados.aceleracao_y = atual.aceleracao_y;
    dados.aceleracao_z = atual.aceleracao_z;
    dados.giroscopio_x = atual.giroscopio_x;
    dados.giroscopio_y = atual.giroscopio_y;
    dados.giroscopio_z = atual.giroscopio_z;
    dados.velocidade_centesimos_kmh = atual.velocidade_centesimos_kmh;
    dados.carga_decimos_percentual = atual.carga_decimos_percentual;
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
    if (ota.validacao_mestre == EstadoValidacaoOta::Validando ||
        ota.validacao_mestre == EstadoValidacaoOta::EmObservacao) {
        dados.estado_mestre = ota.validacao_mestre == EstadoValidacaoOta::Validando
            ? COMANDO_OTA_MESTRE_VALIDANDO : COMANDO_OTA_MESTRE_EM_OBSERVACAO;
        const int64_t duracao = ota.periodo_validacao_fim_us - ota.periodo_validacao_inicio_us;
        if (duracao > 0) {
            const int64_t decorrido = esp_timer_get_time() - ota.periodo_validacao_inicio_us;
            dados.progresso_mestre_decimos = static_cast<uint16_t>(
                std::clamp<int64_t>(decorrido, 0, duracao) * 1000 / duracao);
        }
    } else if (ota.validacao_mestre == EstadoValidacaoOta::Falha) {
        dados.estado_mestre = COMANDO_OTA_MESTRE_VALIDACAO_FALHOU;
        dados.progresso_mestre_decimos = 0;
    } else if (ota.validacao_mestre == EstadoValidacaoOta::Revertida &&
               estado_mestre != EstadoServicoOta::Baixando &&
               estado_mestre != EstadoServicoOta::Aplicado) {
        dados.estado_mestre = COMANDO_OTA_MESTRE_REVERTIDO;
        dados.progresso_mestre_decimos = 0;
    } else if (ota.validacao_mestre == EstadoValidacaoOta::Concluida &&
               estado_mestre != EstadoServicoOta::AguardandoAutorizacao &&
               estado_mestre != EstadoServicoOta::Baixando &&
               estado_mestre != EstadoServicoOta::Aplicado &&
               estado_mestre != EstadoServicoOta::Falha) {
        dados.estado_mestre = COMANDO_OTA_MESTRE_VALIDADO;
        dados.progresso_mestre_decimos = 1000;
    }
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
    if (erro == ESP_ERR_INVALID_RESPONSE) return RESPOSTA_COMANDO_NEGADO;
    if (erro == ESP_ERR_INVALID_STATE || erro == ESP_ERR_TIMEOUT)
        return RESPOSTA_COMANDO_OCUPADO;
    return RESPOSTA_COMANDO_ERRO_INTERNO;
}

void montar_confirmacao_acao(codigo_comando_t comando, uint8_t* resposta,
                            uint16_t* tamanho_resposta) {
    const resposta_comando_acao_t confirmacao = {
        static_cast<uint16_t>(comando), 1, 0,
        static_cast<uint32_t>(esp_timer_get_time() / 1000)};
    std::memcpy(resposta, &confirmacao, sizeof(confirmacao));
    *tamanho_resposta = sizeof(confirmacao);
}

codigo_resposta_comando_t confirmar_acao_mestre(
    acao_mestre_t acao, esp_err_t erro, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t resultado = converter_resultado_acao(erro);
    if (resultado != RESPOSTA_COMANDO_OK) return resultado;
    montar_confirmacao_acao(COMANDO_EXECUTAR_ACAO_MESTRE, resposta,
                           tamanho_resposta);
    ESP_LOGI(ETIQUETA, "Ação operacional remota %u concluída",
             static_cast<unsigned>(acao));
    return RESPOSTA_COMANDO_OK;
}

const char* segundo_texto(const solicitacao_acao_mestre_t& solicitacao) {
    const size_t primeiro = strnlen(solicitacao.texto,
                                   sizeof(solicitacao.texto));
    if (primeiro >= sizeof(solicitacao.texto) - 1) return nullptr;
    const char* segundo = solicitacao.texto + primeiro + 1;
    const size_t restante = sizeof(solicitacao.texto) - primeiro - 1;
    return strnlen(segundo, restante) < restante ? segundo : nullptr;
}

codigo_resposta_comando_t executar_acao_mestre(
    DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta, int64_t recebido_em_us) {
    *tamanho_resposta = 0;
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    if (tamanho_carga != sizeof(solicitacao_acao_mestre_t)) {
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    }
    solicitacao_acao_mestre_t solicitacao{};
    std::memcpy(&solicitacao, carga, sizeof(solicitacao));
    if (solicitacao.acao < ACAO_MESTRE_ALTERNAR_RELATORIOS ||
        solicitacao.acao > ACAO_MESTRE_DEFINIR_AJUSTE_DESENVOLVEDOR) {
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    }
    if (solicitacao.reservado != 0 &&
        !(solicitacao.acao == ACAO_MESTRE_SINCRONIZAR_GOOGLE &&
          solicitacao.reservado ==
              SOLICITACAO_ACAO_CONFIRMACAO_SINCRONIZACAO_DETALHADA)) {
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    }
    if (solicitacao.acao >= ACAO_MESTRE_DEFINIR_MARCHA &&
        solicitacao.texto[sizeof(solicitacao.texto) - 1] != '\0') return RESPOSTA_COMANDO_CARGA_INVALIDA;
    solicitacao.texto[sizeof(solicitacao.texto) - 1] = '\0';
    const auto acao = static_cast<acao_mestre_t>(solicitacao.acao);
    esp_err_t erro = ESP_ERR_NOT_SUPPORTED;
    switch (acao) {
        case ACAO_MESTRE_DEFINIR_MARCHA:
            // A ação antiga representava frente/ré. Reutilizá-la para L/M/H
            // poderia selecionar uma marcha diferente em painéis antigos.
            return RESPOSTA_COMANDO_NEGADO;
        case ACAO_MESTRE_DEFINIR_FAIXA_MARCHA:
            // A faixa é selecionada na chave física. O comando remoto ajusta o limite.
            return RESPOSTA_COMANDO_NAO_SUPORTADO;
        case ACAO_MESTRE_DEFINIR_ACELERACAO_MAXIMA:
            // O limite de aceleração foi retirado da interface de controle.
            return RESPOSTA_COMANDO_NAO_SUPORTADO;
        case ACAO_MESTRE_ALTERNAR_RELATORIOS:
            (void)servico_relatorios_alternar_pausa();
            erro = ESP_OK;
            break;
        case ACAO_MESTRE_ZERAR_PERCURSO:
            erro = servico_odometro_zerar_percurso();
            break;
        case ACAO_MESTRE_SINCRONIZAR_GOOGLE:
            erro = servico_registro_telemetria_solicitar_sincronizacao();
            break;
        case ACAO_MESTRE_DEFINIR_INTERVALO_RELATORIO:
            if (solicitacao.alvo >=
                static_cast<uint8_t>(GrupoRelatorio::Quantidade)) {
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            }
            erro = servico_relatorios_definir_intervalo(
                static_cast<GrupoRelatorio>(solicitacao.alvo),
                solicitacao.valor);
            break;
        case ACAO_MESTRE_DEFINIR_INTERVALO_OTA:
            erro = servico_ota_definir_intervalo_verificacao(
                solicitacao.valor);
            break;
        case ACAO_MESTRE_DEFINIR_INTERVALO_TELEMETRIA_OTA:
            erro = servico_telemetria_definir_intervalo_durante_ota(
                solicitacao.valor);
            break;
        case ACAO_MESTRE_PREPARAR_REDE_WIFI: {
            const char* senha = segundo_texto(solicitacao);
            if (senha == nullptr) senha = "";
            if (!gerenciador_wifi_credenciais_validas(solicitacao.texto, senha)) {
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            }
            rede_wifi_preparada = {};
            copiar_texto(rede_wifi_preparada.ssid,
                         sizeof(rede_wifi_preparada.ssid), solicitacao.texto);
            copiar_texto(rede_wifi_preparada.senha,
                         sizeof(rede_wifi_preparada.senha), senha);
            rede_wifi_preparada.valida = true;
            erro = ESP_OK;
            break;
        }
        case ACAO_MESTRE_CONECTAR_REDE_WIFI_PREPARADA:
            if (!rede_wifi_preparada.valida)
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            erro = servico_wifi_conectar_rede_informada(
                rede_wifi_preparada.ssid, rede_wifi_preparada.senha);
            break;
        case ACAO_MESTRE_SALVAR_REDE_WIFI_PREPARADA:
            if (!rede_wifi_preparada.valida)
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            erro = servico_wifi_salvar_rede_usuario(
                rede_wifi_preparada.ssid, rede_wifi_preparada.senha);
            if (erro == ESP_OK) rede_wifi_preparada = {};
            break;
        case ACAO_MESTRE_CONECTAR_REDE_WIFI_SALVA:
            erro = servico_wifi_conectar_rede_salva(solicitacao.alvo);
            break;
        case ACAO_MESTRE_EXCLUIR_REDE_WIFI_SALVA:
            erro = servico_wifi_excluir_rede_usuario(solicitacao.alvo);
            break;
        case ACAO_MESTRE_DEFINIR_WIFI_AUTOMATICO:
            erro = servico_wifi_definir_conexao_automatica(
                solicitacao.valor != 0, solicitacao.texto);
            break;
        case ACAO_MESTRE_ALTERAR_REDE_WIFI_PRINCIPAL:
            if (!rede_wifi_preparada.valida)
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            erro = servico_wifi_alterar_rede_principal(
                rede_wifi_preparada.ssid, rede_wifi_preparada.senha,
                solicitacao.texto);
            if (erro == ESP_OK) rede_wifi_preparada = {};
            break;
        case ACAO_MESTRE_DEFINIR_REDE_WIFI_SALVA_COMO_PRINCIPAL:
            erro = servico_wifi_definir_rede_salva_como_principal(
                solicitacao.alvo, solicitacao.texto);
            break;
        case ACAO_MESTRE_RESTAURAR_REDE_WIFI_PRINCIPAL:
            erro = servico_wifi_restaurar_rede_principal(solicitacao.texto);
            break;
        case ACAO_MESTRE_DEFINIR_POLITICA_WIFI: {
            PoliticaConexaoWifi politica{};
            politica.tentativas_por_rede =
                static_cast<uint8_t>(solicitacao.valor);
            politica.tempo_por_tentativa_ms = solicitacao.valor_2;
            politica.intervalo_entre_tentativas_ms = solicitacao.valor_3;
            politica.tempo_limite_sessao_ms = solicitacao.valor_4;
            erro = servico_wifi_definir_politica(politica,
                                                  solicitacao.texto);
            break;
        }
        case ACAO_MESTRE_RESTAURAR_POLITICA_WIFI:
            if (solicitacao.alvo || solicitacao.valor || solicitacao.valor_2 ||
                solicitacao.valor_3 || solicitacao.valor_4) {
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            }
            erro = servico_wifi_definir_politica(PoliticaConexaoWifi{}, solicitacao.texto);
            break;
        case ACAO_MESTRE_DEFINIR_AJUSTE_DESENVOLVEDOR:
            if (solicitacao.alvo >= AJUSTES_DESENVOLVEDOR_TOTAL || solicitacao.valor > INT32_MAX ||
                solicitacao.valor_2 || solicitacao.valor_3 || solicitacao.valor_4) {
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            }
            if (!servico_wifi_validar_permissao(solicitacao.texto)) return RESPOSTA_COMANDO_NEGADO;
            erro = ajustes_desenvolvedor_definir(solicitacao.alvo, static_cast<int32_t>(solicitacao.valor));
            break;
        case ACAO_MESTRE_DEFINIR_LOGS_TERMINAL:
            if (solicitacao.valor > ESP_LOG_INFO || solicitacao.valor_2 > AJUSTE_LOG_TERMINAL_FONTE ||
                solicitacao.valor_3 || solicitacao.valor_4) {
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            }
            if (!servico_wifi_validar_permissao(solicitacao.texto)) return RESPOSTA_COMANDO_NEGADO;
            erro = servico_terminal_definir_logs(
                static_cast<AjusteLogsTerminal>(solicitacao.valor_2), solicitacao.alvo,
                static_cast<uint8_t>(solicitacao.valor));
            break;
        case ACAO_MESTRE_DEFINIR_SENSOR:
            if (!CONTROLE_SENSORES_REMOTO_PERMITIDO) return RESPOSTA_COMANDO_NEGADO;
            if ((solicitacao.alvo > SENSOR_MESTRE_TEMPERATURA_ESP &&
                 solicitacao.alvo != SENSOR_MESTRE_TODOS) ||
                solicitacao.valor > 1 || solicitacao.valor_2 || solicitacao.valor_3 ||
                solicitacao.valor_4 || solicitacao.texto[0] != '\0') {
                return RESPOSTA_COMANDO_CARGA_INVALIDA;
            }
            if (solicitacao.alvo == SENSOR_MESTRE_TODOS) {
                servico_aquisicao_definir_todos_sensores(solicitacao.valor == 1);
            } else {
                servico_aquisicao_definir_sensor(
                    static_cast<SensorControlado>(solicitacao.alvo), solicitacao.valor == 1);
            }
            erro = ESP_OK;
            break;
        case ACAO_MESTRE_DEFINIR_LIMITE_MARCHA:
        case ACAO_MESTRE_DEFINIR_LIMITE_ACELERADOR:
        case ACAO_MESTRE_DEFINIR_ZONA_MORTA_PEDAL:
        case ACAO_MESTRE_DEFINIR_TENSAO_MINIMA_DAC:
        case ACAO_MESTRE_DEFINIR_REVERSAO:
        case ACAO_MESTRE_PERMITIR_REVERSAO:
        case ACAO_MESTRE_DEFINIR_VELOCIDADE_MAXIMA:
        case ACAO_MESTRE_DEFINIR_PARADA_REMOTA: {
            if (solicitacao.alvo || solicitacao.valor_2 || solicitacao.valor_3 || solicitacao.valor_4 ||
                std::any_of(solicitacao.texto, solicitacao.texto + sizeof(solicitacao.texto), [](char c){return c != 0;})) {
                erro = ESP_ERR_INVALID_ARG;
                break;
            }
            const auto ajuste = acao == ACAO_MESTRE_DEFINIR_LIMITE_MARCHA ? AjusteControleRemoto::LimiteMarcha :
                acao == ACAO_MESTRE_DEFINIR_LIMITE_ACELERADOR ? AjusteControleRemoto::AceleradorMaximo :
                acao == ACAO_MESTRE_DEFINIR_ZONA_MORTA_PEDAL ? AjusteControleRemoto::ZonaMortaPedal :
                acao == ACAO_MESTRE_DEFINIR_TENSAO_MINIMA_DAC ? AjusteControleRemoto::TensaoMinimaDac :
                acao == ACAO_MESTRE_DEFINIR_REVERSAO ? AjusteControleRemoto::Reversao :
                acao == ACAO_MESTRE_PERMITIR_REVERSAO ? AjusteControleRemoto::PermissaoReversao :
                acao == ACAO_MESTRE_DEFINIR_VELOCIDADE_MAXIMA ? AjusteControleRemoto::VelocidadeMaxima : AjusteControleRemoto::Parada;
            erro = servico_controle_remoto_definir(ajuste, solicitacao.valor, recebido_em_us);
            break;
        }
        case ACAO_MESTRE_DEFINIR_SIMULADOR:
            erro = solicitacao.valor <= 1 && solicitacao.valor_2 == 0 &&
                           solicitacao.valor_3 == 0 && solicitacao.valor_4 == 0 &&
                           solicitacao.texto[0] == '\0'
                       ? servico_simulador_telemetria_definir_ativo(
                             solicitacao.valor == 1)
                       : ESP_ERR_INVALID_ARG;
            break;
    }
    const codigo_resposta_comando_t resultado =
        confirmar_acao_mestre(acao, erro, resposta, tamanho_resposta);
    if (resultado == RESPOSTA_COMANDO_OK &&
        acao == ACAO_MESTRE_SINCRONIZAR_GOOGLE &&
        solicitacao.reservado ==
            SOLICITACAO_ACAO_CONFIRMACAO_SINCRONIZACAO_DETALHADA) {
        resposta_comando_acao_sincronizacao_t confirmacao{};
        std::memcpy(&confirmacao.confirmacao, resposta,
                    sizeof(confirmacao.confirmacao));
        confirmacao.versao_formato =
            COMANDO_FORMATO_CONFIRMACAO_SINCRONIZACAO;
        confirmacao.sincronizacao_id =
            servico_registro_telemetria_obter_estado().sincronizacoes_iniciadas;
        std::memcpy(resposta, &confirmacao, sizeof(confirmacao));
        *tamanho_resposta = sizeof(confirmacao);
    }
    return resultado;
}

codigo_resposta_comando_t responder_logs_terminal(
    DestinoUart origem, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;

    EstadoLogsTerminal estado{};
    const esp_err_t erro = servico_terminal_obter_logs(&estado);
    if (erro != ESP_OK) return converter_resultado_acao(erro);
    resposta_logs_terminal_t dados{};
    dados.versao_formato = COMANDO_FORMATO_LOGS_TERMINAL;
    dados.nivel_global = estado.nivel_global;
    dados.apenas_avisos_projeto = estado.apenas_avisos_projeto;
    dados.cores = estado.cores;
    std::memcpy(dados.niveis_fontes, estado.niveis_fontes, sizeof(dados.niveis_fontes));
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_tarefas_mestre(
    DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta) {
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    if (tamanho_carga != 1 || carga[0] >= COMANDO_MAXIMO_TAREFAS_MESTRE ||
        carga[0] % COMANDO_TAREFAS_MESTRE_POR_PAGINA != 0) {
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    }

    DiagnosticoTarefa tarefas[COMANDO_TAREFAS_MESTRE_POR_PAGINA]{};
    const auto pagina = servico_diagnostico_obter_pagina_tarefas(
        carga[0], tarefas, COMANDO_TAREFAS_MESTRE_POR_PAGINA);
    resposta_tarefas_mestre_t dados{};
    dados.versao_formato = COMANDO_FORMATO_TAREFAS_MESTRE;
    dados.inicio = carga[0];
    dados.total = static_cast<uint8_t>(pagina.total);
    dados.quantidade = static_cast<uint8_t>(pagina.quantidade);
    dados.valido = pagina.valido;
    dados.coletado_em_ms = pagina.coletado_em_ms;
    for (std::size_t i = 0; i < pagina.quantidade; ++i) {
        const auto& origem = tarefas[i];
        auto& destino = dados.tarefas[i];
        copiar_texto(destino.nome, sizeof(destino.nome), origem.nome);
        destino.pilha_reservada_bytes = origem.pilha_reservada_bytes;
        destino.pilha_minima_livre_bytes = origem.pilha_minima_livre_bytes;
        destino.cpu_decimos = origem.cpu_decimos;
        destino.prioridade = static_cast<uint8_t>(origem.prioridade);
        destino.nucleo = static_cast<int8_t>(origem.nucleo);
        destino.estado = origem.estado;
        destino.cpu_valida = origem.cpu_valida;
    }
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_configuracao_mestre(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    const SituacaoOta ota = servico_ota_obter_situacao();
    const EstadoRegistroTelemetria registro =
        servico_registro_telemetria_obter_estado();
    resposta_configuracao_mestre_t dados{};
    dados.relatorios_pausados = servico_relatorios_estao_pausados();
    for (uint8_t indice = 0; indice < 4; ++indice) {
        dados.intervalos_relatorios_ms[indice] =
            servico_relatorios_obter_intervalo(
                static_cast<GrupoRelatorio>(indice));
    }
    dados.intervalo_ota_minutos = ota.intervalo_verificacao_minutos;
    dados.intervalo_telemetria_ota_ms =
        servico_telemetria_obter_intervalo_durante_ota();
    dados.cartao_disponivel = registro.cartao_disponivel;
    dados.amostras_sd = registro.amostras_gravadas;
    dados.arquivos_pendentes = registro.arquivos_pendentes;
    dados.arquivos_enviados = registro.arquivos_enviados;
    dados.quantidade_redes_wifi = wifi.quantidade_redes_usuario;
    dados.wifi_automatico = wifi.conectar_secundarias_automaticamente;
    dados.tentativas_wifi = wifi.politica.tentativas_por_rede;
    dados.tempo_tentativa_wifi_ms = wifi.politica.tempo_por_tentativa_ms;
    dados.intervalo_tentativas_wifi_ms =
        wifi.politica.intervalo_entre_tentativas_ms;
    dados.tempo_limite_wifi_ms = wifi.politica.tempo_limite_sessao_ms;
    dados.principal_alterada = wifi.principal_alterada;
    copiar_texto(dados.rede_principal, sizeof(dados.rede_principal),
                 wifi.rede_principal);
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_simulador_mestre(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    resposta_simulador_mestre_t dados{};
    dados.ativo = servico_simulador_telemetria_esta_ativo();
    dados.padrao_firmware = configuracao::USAR_TELEMETRIA_SIMULADA;
    dados.dinamica_percentual =
        servico_simulador_telemetria_dinamica_percentual();
    dados.amostras_geradas =
        servico_simulador_telemetria_amostras_geradas();
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_sincronizacao_mestre(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;
    const EstadoRegistroTelemetria registro =
        servico_registro_telemetria_obter_estado();
    resposta_sincronizacao_mestre_t dados{};
    dados.versao_formato = COMANDO_FORMATO_SINCRONIZACAO;
    if (registro.iniciado) dados.indicadores |= SINCRONIZACAO_MESTRE_INICIADA;
    if (registro.cartao_disponivel)
        dados.indicadores |= SINCRONIZACAO_MESTRE_CARTAO_DISPONIVEL;
    if (registro.sincronizacao_em_andamento)
        dados.indicadores |= SINCRONIZACAO_MESTRE_EM_ANDAMENTO;
    if (registro.aguardando_confirmacao_upload)
        dados.indicadores |= SINCRONIZACAO_MESTRE_AGUARDANDO_CONFIRMACAO;
    dados.sincronizacoes_iniciadas = registro.sincronizacoes_iniciadas;
    dados.sincronizacoes_finalizadas = registro.sincronizacoes_finalizadas;
    dados.arquivos_pendentes = registro.arquivos_pendentes;
    dados.arquivos_upload_total = registro.arquivos_upload_total;
    dados.arquivos_upload_concluidos = registro.arquivos_upload_concluidos;
    dados.arquivo_atual_bytes_enviados =
        registro.arquivo_atual_bytes_enviados;
    dados.arquivo_atual_bytes_total = registro.arquivo_atual_bytes_total;
    dados.velocidade_upload_bytes_s = registro.velocidade_upload_bytes_s;
    dados.tempo_restante_upload_s = registro.tempo_restante_upload_s;
    dados.ultimo_resultado_sincronizacao =
        static_cast<int32_t>(registro.ultimo_resultado_sincronizacao);
    dados.arquivos_ultima_sincronizacao =
        registro.arquivos_ultima_sincronizacao;
    dados.bytes_ultima_sincronizacao = registro.bytes_ultima_sincronizacao;
    dados.duracao_ultima_sincronizacao_ms =
        registro.duracao_ultima_sincronizacao_ms;
    dados.fim_ultima_sincronizacao_ms =
        registro.fim_ultima_sincronizacao_ms;
    copiar_texto(dados.arquivo_em_upload, sizeof(dados.arquivo_em_upload),
                 registro.arquivo_em_upload);
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_rede_wifi_salva(
    DestinoUart, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta) {
    if (tamanho_carga != sizeof(solicitacao_rede_wifi_salva_t))
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    solicitacao_rede_wifi_salva_t solicitacao{};
    std::memcpy(&solicitacao, carga, sizeof(solicitacao));
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    if (solicitacao.indice >= wifi.quantidade_redes_usuario)
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    resposta_rede_wifi_salva_t dados{};
    dados.indice = solicitacao.indice;
    copiar_texto(dados.ssid, sizeof(dados.ssid),
                 wifi.redes_usuario[solicitacao.indice]);
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_sensores_mestre(
    DestinoUart origem, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    if (tamanho_carga != 0) return RESPOSTA_COMANDO_CARGA_INVALIDA;

    resposta_sensores_mestre_t dados{};
    dados.versao_formato = COMANDO_FORMATO_SENSORES_MESTRE;
    dados.ativos = static_cast<uint8_t>(servico_aquisicao_obter_sensores_ativos());
    dados.controle_permitido = CONTROLE_SENSORES_REMOTO_PERMITIDO;

    // Usa os estados publicados. A consulta nunca acessa o barramento físico.
    DadosTelemetriaVeiculo amostra{};
    const bool real = servico_telemetria_obter_ultima_amostra_local(amostra) && !amostra.simulado;
    if (real && (dados.ativos & (1u << SENSOR_MESTRE_ADC))) {
        const auto sensores = servico_diagnostico_obter_sensores();
        if (sensores.adc.ultimo_erro == ESP_OK) {
            dados.validos |= 1u << SENSOR_MESTRE_ADC;
            dados.adc_bruto = amostra.tensao_adc_bruta;
        }
    }
    if (real && amostra.mpu_ativo && std::isfinite(amostra.temperatura_mpu_c)) {
        dados.validos |= dados.ativos & (1u << SENSOR_MESTRE_MPU6050);
        dados.acelerometro_bruto[0] = amostra.aceleracao_x;
        dados.acelerometro_bruto[1] = amostra.aceleracao_y;
        dados.acelerometro_bruto[2] = amostra.aceleracao_z;
        dados.giroscopio_bruto[0] = amostra.giroscopio_x;
        dados.giroscopio_bruto[1] = amostra.giroscopio_y;
        dados.giroscopio_bruto[2] = amostra.giroscopio_z;
        dados.temperatura_mpu_c = amostra.temperatura_mpu_c;
    }
    if (real && amostra.eletrica_valida && std::isfinite(amostra.tensao_pacote_v) &&
        std::isfinite(amostra.corrente_a) && std::isfinite(amostra.ina_tensao_shunt_mv) &&
        std::isfinite(amostra.ina_temperatura_c)) {
        dados.validos |= dados.ativos & (1u << SENSOR_MESTRE_INA228);
        dados.tensao_ina_v = amostra.tensao_pacote_v;
        dados.corrente_ina_a = amostra.corrente_a;
        dados.queda_shunt_mv = amostra.ina_tensao_shunt_mv;
        dados.temperatura_ina_c = amostra.ina_temperatura_c;
        dados.diagnostico_ina = amostra.ina_diagnostico;
    }
    const auto odometro = servico_odometro_obter_estado();
    if (real && servico_odometro_amostra_recente(odometro, esp_timer_get_time()) &&
        std::isfinite(odometro.roda_esquerda.velocidade_kmh) &&
        std::isfinite(odometro.roda_direita.velocidade_kmh)) {
        dados.validos |= dados.ativos & (1u << SENSOR_MESTRE_ENCODERS);
        dados.contagem_esquerda = odometro.roda_esquerda.contagens_acumuladas;
        dados.contagem_direita = odometro.roda_direita.contagens_acumuladas;
        dados.velocidade_esquerda_kmh = odometro.roda_esquerda.velocidade_kmh;
        dados.velocidade_direita_kmh = odometro.roda_direita.velocidade_kmh;
    }
    const auto saude = servico_diagnostico_obter_resumo();
    if (saude.temperatura_valida) {
        dados.validos |= dados.ativos & (1u << SENSOR_MESTRE_TEMPERATURA_ESP);
        dados.temperatura_esp_centesimos = saude.temperatura_centesimos_c;
    }
    const auto i2c = gerenciador_i2c_obter_estatisticas();
    dados.i2c_operacoes_ok = i2c.operacoes_ok;
    dados.i2c_erros = i2c.erros;
    dados.i2c_sondagens_ok = i2c.sondagens_ok;
    dados.i2c_enderecos_ausentes = i2c.enderecos_ausentes;
    dados.i2c_ultimo_erro = i2c.ultimo_erro;

    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_diagnostico_mestre(
    DestinoUart, const uint8_t*, uint16_t tamanho_carga, uint8_t* resposta,
    uint16_t* tamanho_resposta) {
    const codigo_resposta_comando_t valido = validar_sem_carga(tamanho_carga);
    if (valido != RESPOSTA_COMANDO_OK) return valido;

    const ResumoSaudeSistema saude = servico_diagnostico_obter_resumo();
    const ResumoCpuTarefas cpu = servico_diagnostico_obter_cpu();
    resposta_diagnostico_mestre_t dados{};
    dados.versao_formato = COMANDO_FORMATO_DIAGNOSTICO_MESTRE;
    dados.tarefas_ativas = cpu.quantidade;
    dados.cpu_nucleo_0_decimos_percentual = cpu.nucleo_0_decimos;
    dados.cpu_nucleo_1_decimos_percentual = cpu.nucleo_1_decimos;
    dados.cpu_tarefa_pico_decimos_percentual = cpu.tarefa_pico_decimos;
    copiar_texto(dados.tarefa_cpu_pico, sizeof(dados.tarefa_cpu_pico),
                 cpu.tarefa_pico);
    if (cpu.valido) {
        dados.indicadores |= DIAGNOSTICO_MESTRE_CPU_VALIDO;
        dados.cpu_total_decimos_percentual = static_cast<uint16_t>(
            (cpu.nucleo_0_decimos + cpu.nucleo_1_decimos) / 2U);
    }
    if (saude.temperatura_valida) {
        dados.indicadores |= DIAGNOSTICO_MESTRE_TEMPERATURA_VALIDA;
    }
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) != 0) {
        dados.indicadores |= DIAGNOSTICO_MESTRE_PSRAM_DISPONIVEL;
    }
    if (saude.bytes_pilha_livres != 0) {
        dados.indicadores |= DIAGNOSTICO_MESTRE_PILHA_VALIDA;
    }
    dados.temperatura_centesimos_c = static_cast<int16_t>(std::clamp<int32_t>(
        saude.temperatura_centesimos_c, INT16_MIN, INT16_MAX));
    dados.temperatura_ultimo_erro = saude.temperatura_ultimo_erro;
    dados.tempo_ativo_ms = static_cast<uint32_t>(saude.tempo_ativo_ms);
    dados.heap_livre = saude.heap_livre;
    dados.menor_heap_livre = saude.menor_heap_livre;
    dados.heap_interno_livre = saude.heap_interno_livre;
    dados.menor_heap_interno_livre = saude.menor_heap_interno_livre;
    dados.maior_bloco_heap_interno = saude.maior_bloco_heap_interno;
    dados.menor_maior_bloco_heap_interno =
        saude.menor_maior_bloco_heap_interno;
    dados.psram_livre = saude.psram_livre;
    dados.menor_psram_livre = saude.menor_psram_livre;
    dados.maior_bloco_psram = saude.maior_bloco_psram;
    dados.menor_maior_bloco_psram = saude.menor_maior_bloco_psram;
    dados.menor_pilha_livre_bytes = saude.bytes_pilha_livres;
    dados.causas_ativas = saude.causas_ativas;
    dados.motivo_ultimo_reset = saude.motivo_ultimo_reset;
    dados.erros_sensores = saude.erros_sensores;
    dados.erros_i2c = saude.erros_i2c;
    dados.erros_uart = saude.erros_uart;
    dados.descartes_uart = saude.descartes_uart;
    dados.falhas_ota = saude.falhas_ota;
    dados.falhas_supervisao = saude.falhas_supervisao;
    copiar_texto(dados.tarefa_menor_pilha,
                 sizeof(dados.tarefa_menor_pilha),
                 saude.tarefa_menor_pilha);
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t consultar_satelite_remoto(
    DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta) {
    *tamanho_resposta = 0;
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    if (tamanho_carga != sizeof(solicitacao_consulta_satelite_t))
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    solicitacao_consulta_satelite_t solicitacao{};
    std::memcpy(&solicitacao, carga, sizeof(solicitacao));
    if (((solicitacao.tipo != CONSULTA_SATELITE_CLIENTES_WIFI &&
          solicitacao.reservado != 0) ||
         (solicitacao.tipo == CONSULTA_SATELITE_CLIENTES_WIFI &&
          solicitacao.reservado > 5)) ||
        (solicitacao.alvo != NO_COMANDO_EQUIPE &&
         solicitacao.alvo != NO_COMANDO_VISITANTES) ||
        (solicitacao.tipo != CONSULTA_SATELITE_PING &&
         solicitacao.tipo != CONSULTA_SATELITE_VERSAO &&
         solicitacao.tipo != CONSULTA_SATELITE_DIAGNOSTICO &&
         solicitacao.tipo != CONSULTA_SATELITE_CLIENTES_WIFI)) {
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    }
    const DestinoUart destino = solicitacao.alvo == NO_COMANDO_EQUIPE
                                    ? DestinoUart::Equipe
                                    : DestinoUart::Visitantes;
    const codigo_comando_t comando =
        solicitacao.tipo == CONSULTA_SATELITE_PING ? COMANDO_PING :
        solicitacao.tipo == CONSULTA_SATELITE_VERSAO ? COMANDO_OBTER_VERSAO :
        solicitacao.tipo == CONSULTA_SATELITE_DIAGNOSTICO ?
            COMANDO_OBTER_DIAGNOSTICO_SATELITE : COMANDO_OBTER_CLIENTES_WIFI_SATELITE;
    uint8_t carga_satelite[COMANDO_TAMANHO_MAXIMO_CARGA]{};
    uint16_t tamanho_satelite = 0;
    codigo_resposta_comando_t resultado_satelite = RESPOSTA_COMANDO_TIMEOUT;
    const int64_t inicio_us = esp_timer_get_time();
    const uint8_t inicio = (uint8_t)solicitacao.reservado;
    const esp_err_t erro = servico_comandos_solicitar(
        destino, comando,
        comando == COMANDO_OBTER_CLIENTES_WIFI_SATELITE ? &inicio : nullptr,
        comando == COMANDO_OBTER_CLIENTES_WIFI_SATELITE ? 1 : 0,
        carga_satelite,
        sizeof(carga_satelite), &tamanho_satelite, &resultado_satelite,
        pdMS_TO_TICKS(700));
    if (erro != ESP_OK) {
        return erro == ESP_ERR_TIMEOUT ? RESPOSTA_COMANDO_TIMEOUT
                                       : RESPOSTA_COMANDO_ERRO_INTERNO;
    }
    if (resultado_satelite != RESPOSTA_COMANDO_OK)
        return resultado_satelite;
    if (comando == COMANDO_OBTER_DIAGNOSTICO_SATELITE &&
        tamanho_satelite == sizeof(resposta_diagnostico_satelite_t)) {
        resposta_consulta_diagnostico_satelite_t dados{};
        dados.alvo = solicitacao.alvo;
        dados.tipo = solicitacao.tipo;
        dados.rtt_ms = static_cast<uint32_t>((esp_timer_get_time() - inicio_us) / 1000);
        std::memcpy(&dados.diagnostico, carga_satelite, sizeof(dados.diagnostico));
        std::memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
    if (comando == COMANDO_OBTER_CLIENTES_WIFI_SATELITE &&
        tamanho_satelite == sizeof(resposta_clientes_wifi_satelite_t)) {
        resposta_consulta_clientes_wifi_satelite_t dados{};
        dados.alvo = solicitacao.alvo;
        dados.tipo = solicitacao.tipo;
        dados.rtt_ms = static_cast<uint32_t>((esp_timer_get_time() - inicio_us) / 1000);
        std::memcpy(&dados.situacao, carga_satelite, sizeof(dados.situacao));
        std::memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
    resposta_consulta_satelite_t dados{};
    dados.alvo = solicitacao.alvo;
    dados.tipo = solicitacao.tipo;
    dados.rtt_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - inicio_us) / 1000);
    if (comando == COMANDO_PING &&
        tamanho_satelite == sizeof(resposta_comando_ping_t)) {
        resposta_comando_ping_t ping{};
        std::memcpy(&ping, carga_satelite, sizeof(ping));
        dados.tempo_ativo_ms = ping.tempo_ativo_ms;
    } else if (comando == COMANDO_OBTER_VERSAO &&
               tamanho_satelite == sizeof(resposta_comando_versao_t)) {
        resposta_comando_versao_t versao{};
        std::memcpy(&versao, carga_satelite, sizeof(versao));
        dados.capacidades = versao.capacidades;
        copiar_texto(dados.versao, sizeof(dados.versao), versao.versao);
    } else {
        return RESPOSTA_COMANDO_ERRO_INTERNO;
    }
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t ajustar_radio_satelite(
    DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta) {
    static_assert(sizeof(solicitacao_radio_satelite_t) <= COMANDO_TAMANHO_MAXIMO_CARGA);
    static_assert(sizeof(resposta_radio_satelite_t) <= COMANDO_TAMANHO_MAXIMO_CARGA);
    *tamanho_resposta = 0;
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    if (tamanho_carga != sizeof(solicitacao_radio_satelite_t))
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    solicitacao_radio_satelite_t pedido{};
    std::memcpy(&pedido, carga, sizeof(pedido));
    if ((pedido.alvo != NO_COMANDO_EQUIPE && pedido.alvo != NO_COMANDO_VISITANTES) ||
        pedido.alterar > 1 || pedido.permissao[sizeof(pedido.permissao) - 1] != '\0')
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    if (pedido.alterar && !servico_wifi_validar_permissao(pedido.permissao))
        return RESPOSTA_COMANDO_NEGADO;
    const auto destino = pedido.alvo == NO_COMANDO_EQUIPE ? DestinoUart::Equipe : DestinoUart::Visitantes;
    codigo_resposta_comando_t resultado = RESPOSTA_COMANDO_TIMEOUT;
    uint16_t tamanho = 0;
    const esp_err_t erro = servico_comandos_solicitar(
        destino, COMANDO_AJUSTAR_RADIO_SATELITE,
        pedido.alterar ? &pedido.perfil : nullptr, pedido.alterar ? sizeof(pedido.perfil) : 0,
        resposta, COMANDO_TAMANHO_MAXIMO_CARGA, &tamanho, &resultado, pdMS_TO_TICKS(1000));
    if (erro != ESP_OK) return erro == ESP_ERR_TIMEOUT ? RESPOSTA_COMANDO_TIMEOUT
                                                       : RESPOSTA_COMANDO_ERRO_INTERNO;
    if (resultado != RESPOSTA_COMANDO_OK) return resultado;
    if (tamanho != sizeof(resposta_radio_satelite_t)) return RESPOSTA_COMANDO_ERRO_INTERNO;
    *tamanho_resposta = tamanho;
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t gerir_wifi_satelite_remoto(
    DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta) {
    *tamanho_resposta = 0;
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    if (tamanho_carga != sizeof(solicitacao_gestao_wifi_satelite_t))
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    solicitacao_gestao_wifi_satelite_t pedido{};
    std::memcpy(&pedido, carga, sizeof(pedido));
    if (pedido.alvo != NO_COMANDO_VISITANTES ||
        (pedido.acao != GESTAO_WIFI_SATELITE_LIMITE &&
         pedido.acao != GESTAO_WIFI_SATELITE_DESCONECTAR) ||
        (pedido.acao == GESTAO_WIFI_SATELITE_LIMITE &&
         pedido.limite_sessao_minutos > 1440))
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    codigo_resposta_comando_t resultado = RESPOSTA_COMANDO_TIMEOUT;
    uint16_t tamanho = 0;
    const esp_err_t erro = servico_comandos_solicitar(
        DestinoUart::Visitantes, COMANDO_GERIR_WIFI_SATELITE,
        &pedido, sizeof(pedido), resposta, COMANDO_TAMANHO_MAXIMO_CARGA,
        &tamanho, &resultado, pdMS_TO_TICKS(1000));
    if (erro != ESP_OK) return erro == ESP_ERR_TIMEOUT ? RESPOSTA_COMANDO_TIMEOUT
                                                        : RESPOSTA_COMANDO_ERRO_INTERNO;
    if (resultado != RESPOSTA_COMANDO_OK) return resultado;
    if (tamanho != sizeof(resposta_gestao_wifi_satelite_t))
        return RESPOSTA_COMANDO_ERRO_INTERNO;
    *tamanho_resposta = tamanho;
    return RESPOSTA_COMANDO_OK;
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
        case COMANDO_AUTORIZAR_OTA_TODOS:
            erro = servico_ota_autorizar_atualizacao_todos();
            break;
        case COMANDO_CANCELAR_OTA:
            erro = servico_ota_cancelar_atualizacao();
            break;
        default:
            break;
    }
    const codigo_resposta_comando_t resultado = converter_resultado_acao(erro);
    if (resultado == RESPOSTA_COMANDO_OK) {
        montar_confirmacao_acao(comando, resposta, tamanho_resposta);
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
DEFINIR_ACAO_OTA(autorizar_ota_todos, COMANDO_AUTORIZAR_OTA_TODOS)
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
    servico_relatorios_solicitar_resumo();
    montar_confirmacao_acao(COMANDO_SOLICITAR_RESUMO_TERMINAL, resposta,
                           tamanho_resposta);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_controle_remoto(
    DestinoUart origem, const uint8_t* carga, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta) {
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    if (tamanho_carga != 0 && (tamanho_carga != 1 ||
        (carga[0] != CONTROLE_REMOTO_FORMATO && carga[0] != CONTROLE_REMOTO_FORMATO_REARME)))
        return RESPOSTA_COMANDO_CARGA_INVALIDA;
    auto dados = servico_controle_remoto_obter_estado();
    static_assert(sizeof(dados) <= COMANDO_TAMANHO_MAXIMO_CARGA, "Estado remoto excede quadro");
    if (tamanho_carga == 0) {
        dados.formato = CONTROLE_REMOTO_FORMATO_LEGADO;
        *tamanho_resposta = CONTROLE_REMOTO_TAMANHO_LEGADO;
    } else if (carga[0] == CONTROLE_REMOTO_FORMATO_REARME) {
        dados.formato = CONTROLE_REMOTO_FORMATO_REARME;
        *tamanho_resposta = CONTROLE_REMOTO_TAMANHO_REARME;
    } else *tamanho_resposta = sizeof(dados);
    std::memcpy(resposta, &dados, *tamanho_resposta);
    return RESPOSTA_COMANDO_OK;
}

codigo_resposta_comando_t responder_ajustes_desenvolvedor(
    DestinoUart origem, const uint8_t*, uint16_t tamanho_carga,
    uint8_t* resposta, uint16_t* tamanho_resposta) {
    if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
    if (tamanho_carga != 0) return RESPOSTA_COMANDO_CARGA_INVALIDA;
    const auto dados = ajustes_desenvolvedor_consultar();
    static_assert(sizeof(dados) <= COMANDO_TAMANHO_MAXIMO_CARGA);
    std::memcpy(resposta, &dados, sizeof(dados));
    *tamanho_resposta = sizeof(dados);
    return RESPOSTA_COMANDO_OK;
}

constexpr RegistroComando COMANDOS[] = {
    {COMANDO_OBTER_AJUSTES_DESENVOLVEDOR_EXTRAS, [](DestinoUart origem,
        const uint8_t* carga, uint16_t tamanho_carga, uint8_t* resposta, uint16_t* tamanho_resposta) {
        if (origem != DestinoUart::Equipe) return RESPOSTA_COMANDO_NEGADO;
        if (tamanho_carga != 0 &&
            (tamanho_carga != 1 || carga[0] != AJUSTES_EXTRAS_FORMATO_COM_LED))
            return RESPOSTA_COMANDO_CARGA_INVALIDA;
        const auto dados = ajustes_desenvolvedor_consultar_extras();
        static_assert(sizeof(dados) <= COMANDO_TAMANHO_MAXIMO_CARGA);
        *tamanho_resposta = tamanho_carga == 0 ? AJUSTES_EXTRAS_TAMANHO_LEGADO : sizeof(dados);
        std::memcpy(resposta, &dados, *tamanho_resposta);
        return RESPOSTA_COMANDO_OK;
    }},
    {COMANDO_OBTER_AJUSTES_DESENVOLVEDOR, responder_ajustes_desenvolvedor},
    {COMANDO_OBTER_CONTROLE_REMOTO, responder_controle_remoto},
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
    {COMANDO_AUTORIZAR_OTA_TODOS, autorizar_ota_todos},
    {COMANDO_CANCELAR_OTA, cancelar_ota},
    {COMANDO_SOLICITAR_RESUMO_TERMINAL, solicitar_resumo_terminal},
    {COMANDO_OBTER_CONFIGURACAO_MESTRE, responder_configuracao_mestre},
    {COMANDO_OBTER_REDE_WIFI_SALVA, responder_rede_wifi_salva},
    {COMANDO_CONSULTAR_SATELITE, consultar_satelite_remoto},
    {COMANDO_GERIR_WIFI_SATELITE, gerir_wifi_satelite_remoto},
    {COMANDO_AJUSTAR_RADIO_SATELITE, ajustar_radio_satelite},
    {COMANDO_OBTER_DIAGNOSTICO_MESTRE, responder_diagnostico_mestre},
    {COMANDO_OBTER_SENSORES_MESTRE, responder_sensores_mestre},
    {COMANDO_OBTER_LOGS_TERMINAL, responder_logs_terminal},
    {COMANDO_OBTER_TAREFAS_MESTRE, responder_tarefas_mestre},
    {COMANDO_OBTER_ESTADO_SINCRONIZACAO, responder_sincronizacao_mestre},
    {COMANDO_OBTER_ESTADO_SIMULADOR, responder_simulador_mestre},
};

const RegistroComando* localizar_comando(uint16_t codigo) {
    for (const auto& comando : COMANDOS) {
        if (comando.codigo == codigo) return &comando;
    }
    return nullptr;
}

}  // namespace

codigo_resposta_comando_t comandos_mestre_despachar(
    uint16_t comando, DestinoUart origem, const uint8_t* carga,
    uint16_t tamanho_carga, uint8_t* resposta, uint16_t* tamanho_resposta,
    int64_t recebido_em_us) {
    if (comando == COMANDO_EXECUTAR_ACAO_MESTRE) {
        return executar_acao_mestre(origem, carga, tamanho_carga, resposta,
                                    tamanho_resposta, recebido_em_us);
    }
    const RegistroComando* registro = localizar_comando(comando);
    if (registro == nullptr) return RESPOSTA_COMANDO_NAO_SUPORTADO;
    return registro->executar(origem, carga, tamanho_carga, resposta,
                              tamanho_resposta);
}
