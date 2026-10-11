// Atualiza o painel local com estado, telemetria e progresso OTA.
#include "servicos/servico_painel_local.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dispositivos/tela_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nucleo/configuracao_placa.h"
#include "servicos/servico_diagnostico.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_sinalizacao.h"
#include "servicos/servico_telemetria.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "painel_local";
constexpr std::size_t MAXIMO_LINHAS = 8;
constexpr std::size_t TAMANHO_LINHA = 22;
constexpr uint32_t INTERVALO_REPETICAO_ERRO_MS = 5000;
constexpr uint32_t INTERVALO_PAGINA_SAUDAVEL_MS = 6000;
constexpr uint32_t INTERVALO_ALTERNANCIA_ALERTA_MS = 3000;
constexpr uint32_t DURACAO_INICIAL_ESTADO_MS = 8000;
constexpr uint32_t INTERVALO_LEMBRETE_OTA_MS = 30000;
constexpr uint32_t DURACAO_LEMBRETE_OTA_MS = 6000;
constexpr uint32_t INTERVALO_LEMBRETE_AVISO_MS = 16000;
constexpr uint32_t DURACAO_LEMBRETE_AVISO_MS = 7000;
constexpr uint32_t INTERVALO_LEMBRETE_FALHA_MS = 14000;
constexpr uint32_t DURACAO_LEMBRETE_FALHA_MS = 10000;
constexpr UBaseType_t CAPACIDADE_FILA_MENSAGENS = 10;
constexpr uint32_t CAUSAS_EXIBIVEIS[] = {
    CausaSaudeHeapCritico,
    CausaSaudePilhaCritica,
    CausaSaudeSensor,
    CausaSaudeI2c,
    CausaSaudeUart,
    CausaSaudeSupervisao,
    CausaSaudeEquipeSemResposta,
    CausaSaudeVisitantesSemResposta,
    CausaSaudeWifiFraco,
    CausaSaudeOtaRecente,
    CausaSaudePersistencia,
};

struct SituacaoPainelLocal {
    bool iniciado = false;
    bool tela_disponivel = false;
    EstadoSinalizacao ultimo_estado_exibido = EstadoSinalizacao::Desligado;
    uint32_t atualizacoes_ok = 0;
    uint32_t falhas_atualizacao = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

struct EtapaInicializacaoPainel {
    char descricao[TAMANHO_LINHA] = "PREPARANDO SERVICOS";
    uint8_t percentual = 0;
    esp_err_t resultado = ESP_OK;
};

struct MensagemPainelLocal {
    TipoMensagemPainelLocal tipo = TipoMensagemPainelLocal::Informacao;
    char titulo[TAMANHO_LINHA]{};
    char detalhe[TAMANHO_LINHA]{};
    uint32_t duracao_ms = 5000;
};

struct MensagemPainelAtiva {
    MensagemPainelLocal mensagem{};
    int64_t expira_em_ms = 0;
    bool ativa = false;
};

portMUX_TYPE trava_situacao = portMUX_INITIALIZER_UNLOCKED;
SituacaoPainelLocal situacao{};
EtapaInicializacaoPainel etapa_inicializacao{};
TaskHandle_t tarefa_painel_handle = nullptr;
QueueHandle_t fila_mensagens = nullptr;

using LinhasPainel = char[MAXIMO_LINHAS][TAMANHO_LINHA];

void copiar_linha(char* destino, const char* origem) {
    if (destino == nullptr) return;
    std::snprintf(destino, TAMANHO_LINHA, "%.21s", origem == nullptr ? "" : origem);
}

EtapaInicializacaoPainel obter_etapa_inicializacao() {
    portENTER_CRITICAL(&trava_situacao);
    const EtapaInicializacaoPainel copia = etapa_inicializacao;
    portEXIT_CRITICAL(&trava_situacao);
    return copia;
}

std::size_t montar_inicializacao(LinhasPainel& linhas,
                                 int* progresso_decimos) {
    const EtapaInicializacaoPainel etapa = obter_etapa_inicializacao();
    copiar_linha(linhas[0], "UFSM-CS | PARTIDA");
    copiar_linha(linhas[1], "---------------------");
    copiar_linha(linhas[2], etapa.resultado == ESP_OK
                                ? "INICIALIZANDO"
                                : "INICIALIZACAO DEGRADADA");
    copiar_linha(linhas[3], etapa.descricao);
    std::snprintf(linhas[5], TAMANHO_LINHA, "PROGRESSO: %hhu%%",
                  etapa.percentual);
    if (etapa.resultado == ESP_OK) {
        copiar_linha(linhas[6], "VERIFICANDO MODULOS");
    } else {
        std::snprintf(linhas[6], TAMANHO_LINHA, "ERRO: %.14s",
                      esp_err_to_name(etapa.resultado));
    }
    *progresso_decimos = static_cast<int>(etapa.percentual) * 10;
    return 7;
}

std::size_t montar_mensagem(const MensagemPainelLocal& mensagem,
                            LinhasPainel& linhas) {
    const char* categoria = "INFORMACAO";
    switch (mensagem.tipo) {
        case TipoMensagemPainelLocal::Sucesso: categoria = "CONCLUIDO"; break;
        case TipoMensagemPainelLocal::Aviso: categoria = "ATENCAO"; break;
        case TipoMensagemPainelLocal::Erro: categoria = "ERRO"; break;
        case TipoMensagemPainelLocal::Informacao: break;
    }
    copiar_linha(linhas[0], categoria);
    copiar_linha(linhas[1], "---------------------");
    copiar_linha(linhas[3], mensagem.titulo);
    copiar_linha(linhas[4], mensagem.detalhe);
    copiar_linha(linhas[6], mensagem.tipo == TipoMensagemPainelLocal::Erro
                                ? "CONSULTE O TERMINAL"
                                : "SISTEMA EM OPERACAO");
    return 7;
}

bool janela_recorrente_ativa(int64_t decorrido_ms, uint32_t duracao_inicial_ms,
                             uint32_t intervalo_ms, uint32_t duracao_ms) {
    if (decorrido_ms < static_cast<int64_t>(duracao_inicial_ms)) return true;
    if (intervalo_ms == 0 || duracao_ms == 0) return false;
    const uint32_t fase = static_cast<uint32_t>(
        (decorrido_ms - duracao_inicial_ms) % intervalo_ms);
    return fase < duracao_ms;
}

EstadoSinalizacao selecionar_estado_exibicao(EstadoSinalizacao estado,
                                              int64_t instante_ms) {
    static EstadoSinalizacao estado_observado = EstadoSinalizacao::Desligado;
    static int64_t inicio_estado_ms = 0;
    if (estado != estado_observado) {
        estado_observado = estado;
        inicio_estado_ms = instante_ms;
    }
    const int64_t decorrido_ms = instante_ms - inicio_estado_ms;
    switch (estado) {
        case EstadoSinalizacao::OtaAguardandoAutorizacao:
            return janela_recorrente_ativa(
                       decorrido_ms, DURACAO_INICIAL_ESTADO_MS,
                       INTERVALO_LEMBRETE_OTA_MS, DURACAO_LEMBRETE_OTA_MS)
                       ? estado : EstadoSinalizacao::Saudavel;
        case EstadoSinalizacao::OtaConcluido:
            return decorrido_ms < DURACAO_INICIAL_ESTADO_MS
                       ? estado : EstadoSinalizacao::Saudavel;
        case EstadoSinalizacao::OtaFalha:
            return janela_recorrente_ativa(
                       decorrido_ms, DURACAO_INICIAL_ESTADO_MS,
                       INTERVALO_LEMBRETE_OTA_MS, DURACAO_LEMBRETE_OTA_MS)
                       ? estado : EstadoSinalizacao::Saudavel;
        case EstadoSinalizacao::Aviso:
            return janela_recorrente_ativa(
                       decorrido_ms, DURACAO_INICIAL_ESTADO_MS,
                       INTERVALO_LEMBRETE_AVISO_MS,
                       DURACAO_LEMBRETE_AVISO_MS)
                       ? estado : EstadoSinalizacao::Saudavel;
        case EstadoSinalizacao::Falha:
            return janela_recorrente_ativa(
                       decorrido_ms, DURACAO_INICIAL_ESTADO_MS,
                       INTERVALO_LEMBRETE_FALHA_MS,
                       DURACAO_LEMBRETE_FALHA_MS)
                       ? estado : EstadoSinalizacao::Saudavel;
        case EstadoSinalizacao::WifiAtivo:
            return janela_recorrente_ativa(
                       decorrido_ms, 5000, 30000, 5000)
                       ? estado : EstadoSinalizacao::Saudavel;
        default:
            return estado;
    }
}

bool estado_reserva_tela(EstadoSinalizacao estado) {
    return estado == EstadoSinalizacao::Inicializando ||
           estado == EstadoSinalizacao::OtaVerificando ||
           estado == EstadoSinalizacao::OtaBaixando ||
           estado == EstadoSinalizacao::Falha;
}

void detectar_conclusao_ota() {
    static uint32_t conclusoes_observadas = 0;
    const SituacaoOta ota = servico_ota_obter_situacao();
    if (ota.conclusoes == conclusoes_observadas) return;
    conclusoes_observadas = ota.conclusoes;
    char detalhe[TAMANHO_LINHA]{};
    std::snprintf(detalhe, sizeof(detalhe), "ALVO: %.14s",
                  servico_ota_nome_alvo(ota.ultimo_alvo_concluido));
    (void)servico_painel_local_notificar(
        TipoMensagemPainelLocal::Sucesso, "OTA CONCLUIDA", detalhe, 6000);
}

void montar_estado_links(char* destino, const ResumoSaudeSistema& saude) {
    std::snprintf(destino, TAMANHO_LINHA, "EQP:%s  VIS:%s",
                  saude.equipe_respondendo ? "OK" : "--",
                  saude.visitantes_respondendo ? "OK" : "--");
}

std::size_t contar_causas(uint32_t causas) {
    std::size_t quantidade = 0;
    for (const uint32_t causa : CAUSAS_EXIBIVEIS) {
        if ((causas & causa) != 0) quantidade++;
    }
    return quantidade;
}

uint32_t obter_causa(uint32_t causas, std::size_t indice_desejado) {
    std::size_t indice = 0;
    for (const uint32_t causa : CAUSAS_EXIBIVEIS) {
        if ((causas & causa) == 0) continue;
        if (indice == indice_desejado) return causa;
        indice++;
    }
    return CausaSaudeNenhuma;
}

std::size_t montar_alerta(LinhasPainel& linhas, EstadoSinalizacao estado,
                          const ResumoSaudeSistema& saude,
                          const SituacaoOta& ota) {
    uint32_t causas = saude.causas_ativas;
    if (estado == EstadoSinalizacao::OtaFalha && ota.ultimo_erro != ESP_OK) {
        causas |= CausaSaudeOtaRecente;
    }
    const std::size_t quantidade = contar_causas(causas);
    const int64_t instante_ms = esp_timer_get_time() / 1000;
    const std::size_t indice = quantidade == 0
        ? 0
        : static_cast<std::size_t>(
              instante_ms / INTERVALO_ALTERNANCIA_ALERTA_MS) % quantidade;
    const uint32_t causa = quantidade == 0
                               ? CausaSaudeNenhuma
                               : obter_causa(causas, indice);
    const bool falha = estado == EstadoSinalizacao::Falha ||
                       estado == EstadoSinalizacao::OtaFalha;
    copiar_linha(linhas[0], falha ? "FALHA DO SISTEMA" : "AVISO DO SISTEMA");
    copiar_linha(linhas[1], "---------------------");
    copiar_linha(linhas[6], "CONSULTE O TERMINAL");

    switch (causa) {
        case CausaSaudeHeapCritico:
            copiar_linha(linhas[2], "MEMORIA INTERNA BAIXA");
            std::snprintf(linhas[3], TAMANHO_LINHA, "LIVRE: %lu KB",
                          static_cast<unsigned long>(
                              saude.heap_interno_livre / 1024u));
            std::snprintf(linhas[4], TAMANHO_LINHA, "BLOCO: %lu KB",
                          static_cast<unsigned long>(
                              saude.maior_bloco_heap_interno / 1024u));
            break;
        case CausaSaudePilhaCritica:
            copiar_linha(linhas[2], "PILHA QUASE ESGOTADA");
            std::snprintf(linhas[3], TAMANHO_LINHA, "LIVRE: %lu BYTES",
                          static_cast<unsigned long>(
                              saude.bytes_pilha_livres));
            copiar_linha(linhas[4], "RISCO DE REINICIO");
            break;
        case CausaSaudeSensor:
            copiar_linha(linhas[2], "SENSOR COM FALHA");
            std::snprintf(linhas[3], TAMANHO_LINHA, "ERROS: %lu",
                          static_cast<unsigned long>(saude.erros_sensores));
            copiar_linha(linhas[4], "VERIFIQUE CONEXOES");
            break;
        case CausaSaudeI2c:
            copiar_linha(linhas[2], "FALHA NO BARRAMENTO I2C");
            std::snprintf(linhas[3], TAMANHO_LINHA, "ERROS: %lu",
                          static_cast<unsigned long>(saude.erros_i2c));
            copiar_linha(linhas[4], "MPU/TELA NO MESMO BUS");
            break;
        case CausaSaudeUart:
            copiar_linha(linhas[2], "FALHA NA UART");
            std::snprintf(linhas[3], TAMANHO_LINHA, "ERROS: %lu",
                          static_cast<unsigned long>(saude.erros_uart));
            std::snprintf(linhas[4], TAMANHO_LINHA, "DESCARTES: %lu",
                          static_cast<unsigned long>(saude.descartes_uart));
            break;
        case CausaSaudeSupervisao:
            copiar_linha(linhas[2], "TAREFA SEM RESPOSTA");
            std::snprintf(linhas[3], TAMANHO_LINHA, "FALHAS: %lu",
                          static_cast<unsigned long>(saude.falhas_supervisao));
            copiar_linha(linhas[4], "RECUPERACAO EM CURSO");
            break;
        case CausaSaudeEquipeSemResposta:
            copiar_linha(linhas[2], "EQUIPE SEM RESPOSTA");
            std::snprintf(linhas[3], TAMANHO_LINHA, "ULTIMO RTT: %lu MS",
                          static_cast<unsigned long>(saude.latencia_equipe_ms));
            copiar_linha(linhas[4], "VERIFIQUE UART E ALIM");
            break;
        case CausaSaudeVisitantesSemResposta:
            copiar_linha(linhas[2], "VISITANTES SEM RESP.");
            std::snprintf(linhas[3], TAMANHO_LINHA, "ULTIMO RTT: %lu MS",
                          static_cast<unsigned long>(
                              saude.latencia_visitantes_ms));
            copiar_linha(linhas[4], "VERIFIQUE UART E ALIM");
            break;
        case CausaSaudeWifiFraco:
            copiar_linha(linhas[2], "SINAL WIFI FRACO");
            std::snprintf(linhas[3], TAMANHO_LINHA, "RSSI: %hhd DBM",
                          saude.wifi_rssi_dbm);
            copiar_linha(linhas[4], "APROXIME O ROTEADOR");
            break;
        case CausaSaudeOtaRecente:
            copiar_linha(linhas[2], "ATUALIZACAO FALHOU");
            std::snprintf(linhas[3], TAMANHO_LINHA, "ERRO: %.14s",
                          esp_err_to_name(ota.ultimo_erro));
            std::snprintf(linhas[4], TAMANHO_LINHA, "ALVO: %.12s",
                          servico_ota_nome_alvo(ota.alvo_ativo));
            break;
        case CausaSaudePersistencia:
            copiar_linha(linhas[2], "FALHA AO SALVAR DADOS");
            copiar_linha(linhas[3], "ODOMETRO NAO PERSISTIU");
            copiar_linha(linhas[4], "FLASH/NVS EM ATENCAO");
            break;
        default:
            copiar_linha(linhas[2], falha ? "FALHA CRITICA" : "ATENCAO");
            copiar_linha(linhas[3], "EVENTO SEM DETALHES");
            copiar_linha(linhas[4], "VERIFIQUE OS REGISTROS");
            break;
    }
    if (quantidade > 1) {
        std::snprintf(linhas[7], TAMANHO_LINHA, "ALERTA %hhu DE %hhu",
                      static_cast<uint8_t>(indice + 1),
                      static_cast<uint8_t>(quantidade));
    } else {
        copiar_linha(linhas[7], "ALERTA ATIVO");
    }
    return 8;
}

uint16_t limitar_uint16(uint32_t valor) {
    return valor > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(valor);
}

uint16_t converter_decimos(float valor, uint16_t limite) {
    if (!(valor > 0.0f)) return 0;
    const float escalado = valor * 10.0f;
    if (escalado >= static_cast<float>(limite)) return limite;
    return static_cast<uint16_t>(escalado + 0.5f);
}

int16_t limitar_inteiro(float valor) {
    if (valor > static_cast<float>(INT16_MAX)) return INT16_MAX;
    if (valor < static_cast<float>(INT16_MIN)) return INT16_MIN;
    return static_cast<int16_t>(valor);
}

const char* nome_marcha(MarchaVeiculo marcha) {
    switch (marcha) {
        case MarchaVeiculo::Estacionado: return "P";
        case MarchaVeiculo::Neutro: return "N";
        case MarchaVeiculo::Frente: return "D";
        case MarchaVeiculo::Re: return "R";
    }
    return "?";
}

const char* nome_fase_verificacao_ota(EstadoServicoOta estado) {
    switch (estado) {
        case EstadoServicoOta::Inicializando: return "INICIALIZANDO";
        case EstadoServicoOta::AguardandoRede: return "CONECTANDO WIFI";
        case EstadoServicoOta::Verificando: return "MANIFESTO HTTPS";
        default: return "PROCESSANDO";
    }
}

const char* nome_estado_satelite_curto(EstadoOtaSatelite estado) {
    switch (estado) {
        case EstadoOtaSatelite::Atualizado: return "OK";
        case EstadoOtaSatelite::AguardandoAutorizacao: return "NOVA";
        case EstadoOtaSatelite::Indisponivel: return "OFF";
        case EstadoOtaSatelite::NaoSuportado: return "N/A";
        case EstadoOtaSatelite::Falha: return "ERRO";
        case EstadoOtaSatelite::Transferindo: return "ENV";
        case EstadoOtaSatelite::Reiniciando: return "RST";
        case EstadoOtaSatelite::Concluido: return "OK";
        case EstadoOtaSatelite::Cancelado: return "CANC";
        case EstadoOtaSatelite::Desconhecido: return "?";
    }
    return "?";
}

void montar_linha_versao_satelite(char* destino, const char* prefixo,
                                  const SituacaoOtaSatelite& satelite) {
    std::snprintf(destino, TAMANHO_LINHA, "%.3s:%.4s %.5s>%.5s", prefixo,
                  nome_estado_satelite_curto(satelite.estado),
                  satelite.versao_atual[0] ? satelite.versao_atual : "-",
                  satelite.versao_disponivel[0] ? satelite.versao_disponivel : "-");
}

std::size_t montar_pagina_saudavel(LinhasPainel& linhas,
                                   const ResumoSaudeSistema& saude,
                                   const SituacaoOta& ota,
                                   const ResumoWifi& wifi) {
    const uint8_t pagina = static_cast<uint8_t>(
        (saude.tempo_ativo_ms / INTERVALO_PAGINA_SAUDAVEL_MS) % 3u);
    copiar_linha(linhas[1], "---------------------");

    if (pagina == 0) {
        copiar_linha(linhas[0], "UFSM-CS  MESTRE");
        copiar_linha(linhas[2], "SISTEMA: OK");
        montar_estado_links(linhas[3], saude);
        const uint16_t horas = limitar_uint16(
            static_cast<uint32_t>(saude.tempo_ativo_ms / 3600000u));
        const uint8_t minutos = static_cast<uint8_t>(
            (saude.tempo_ativo_ms / 60000u) % 60u);
        std::snprintf(linhas[4], TAMANHO_LINHA, "ATIVO: %huH %02hhuM",
                      horas, minutos);
        std::snprintf(linhas[5], TAMANHO_LINHA, "RAM:%huK PS:%huK",
                      limitar_uint16(saude.heap_interno_livre / 1024u),
                      limitar_uint16(saude.psram_livre / 1024u));
        std::snprintf(linhas[6], TAMANHO_LINHA, "FW: %.17s",
                      ota.versao_atual[0] ? ota.versao_atual : "-");
        copiar_linha(linhas[7], "STATUS  1/3");
        return 8;
    }

    if (pagina == 1) {
        const DadosTelemetriaVeiculo dados =
            servico_telemetria_obter_ultimos_dados();
        const EstatisticasTelemetria telemetria =
            servico_telemetria_obter_estatisticas();
        const uint16_t velocidade = converter_decimos(dados.velocidade_kmh, 9999u);
        const uint16_t carga = converter_decimos(dados.carga_percentual, 1000u);
        copiar_linha(linhas[0], "TELEMETRIA");
        std::snprintf(linhas[2], TAMANHO_LINHA, "VEL: %hu.%hu KM/H",
                      static_cast<uint16_t>(velocidade / 10u),
                      static_cast<uint16_t>(velocidade % 10u));
        std::snprintf(linhas[3], TAMANHO_LINHA, "BATERIA: %hu.%hu%%",
                      static_cast<uint16_t>(carga / 10u),
                      static_cast<uint16_t>(carga % 10u));
        std::snprintf(linhas[4], TAMANHO_LINHA, "TENSAO: %hu.%hu V",
                      static_cast<uint16_t>(converter_decimos(
                          dados.tensao_pacote_v, 9999u) / 10u),
                      static_cast<uint16_t>(converter_decimos(
                          dados.tensao_pacote_v, 9999u) % 10u));
        std::snprintf(linhas[5], TAMANHO_LINHA, "POTENCIA: %hd W",
                      limitar_inteiro(dados.potencia_w));
        std::snprintf(linhas[6], TAMANHO_LINHA, "MARCHA: %.1s  SEQ:%hu",
                      nome_marcha(dados.marcha), telemetria.ultima_sequencia);
        copiar_linha(linhas[7], dados.simulado ? "SIMULADA  2/3" : "REAL  2/3");
        return 8;
    }

    copiar_linha(linhas[0], "COMUNICACAO");
    montar_estado_links(linhas[2], saude);
    std::snprintf(linhas[3], TAMANHO_LINHA, "RTT EQP: %hu MS",
                  limitar_uint16(saude.latencia_equipe_ms));
    std::snprintf(linhas[4], TAMANHO_LINHA, "RTT VIS: %hu MS",
                  limitar_uint16(saude.latencia_visitantes_ms));
    if (wifi.conectado) {
        std::snprintf(linhas[5], TAMANHO_LINHA, "WIFI: %hhd DBM",
                      wifi.rssi_dbm);
    } else {
        copiar_linha(linhas[5], wifi.radio_ativo ? "WIFI: PROCURANDO" :
                                                  "WIFI: REPOUSO");
    }
    copiar_linha(linhas[6], "UART: 1 MBPS");
    copiar_linha(linhas[7], "ENLACES  3/3");
    return 8;
}

void montar_progresso_ota(char* destino,
                          const ProgressoTransferenciaOta& progresso) {
    const uint32_t decimos_limitados = progresso.percentual_decimos > 1000u
                                           ? 1000u
                                           : progresso.percentual_decimos;
    const uint32_t recebidos_kib = progresso.bytes_transferidos / 1024u;
    const uint32_t total_kib = progresso.tamanho_total / 1024u;
    const uint16_t recebidos_exibidos = recebidos_kib > UINT16_MAX
                                            ? UINT16_MAX
                                            : static_cast<uint16_t>(recebidos_kib);
    const uint16_t total_exibido = total_kib > UINT16_MAX
                                        ? UINT16_MAX
                                        : static_cast<uint16_t>(total_kib);
    std::snprintf(destino, TAMANHO_LINHA, "%hhu.%hhu%% %hu/%huK",
                  static_cast<uint8_t>(decimos_limitados / 10u),
                  static_cast<uint8_t>(decimos_limitados % 10u),
                  recebidos_exibidos, total_exibido);
}

std::size_t montar_conteudo(EstadoSinalizacao estado, LinhasPainel& linhas,
                            int* progresso_decimos) {
    std::memset(linhas, 0, sizeof(LinhasPainel));
    *progresso_decimos = -1;
    copiar_linha(linhas[0], "UFSM-CS | MESTRE");

    if (estado == EstadoSinalizacao::Inicializando) {
        return montar_inicializacao(linhas, progresso_decimos);
    }
    if (estado == EstadoSinalizacao::Desligado) {
        copiar_linha(linhas[3], "PAINEL EM ESPERA");
        return 4;
    }

    const SituacaoOta ota = servico_ota_obter_situacao();
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    const ResumoSaudeSistema saude = servico_diagnostico_obter_resumo();
    if (estado == EstadoSinalizacao::Falha) {
        return montar_alerta(linhas, estado, saude, ota);
    }
    switch (estado) {
        case EstadoSinalizacao::Saudavel:
            return montar_pagina_saudavel(linhas, saude, ota, wifi);
        case EstadoSinalizacao::WifiAtivo:
            copiar_linha(linhas[0], "WIFI | SESSAO");
            copiar_linha(linhas[1], wifi.conectado ? "STATUS: CONECTADO" :
                                                    "STATUS: PROCURANDO");
            std::snprintf(linhas[2], TAMANHO_LINHA, "REDE: %.15s",
                          wifi.rede_ativa[0] ? wifi.rede_ativa : "-");
            if (wifi.conectado) {
                std::snprintf(linhas[3], TAMANHO_LINHA, "SINAL:%hhdDBM CH:%hhu",
                              wifi.rssi_dbm, wifi.canal);
            } else {
                copiar_linha(linhas[3], "SINAL: AGUARDANDO");
            }
            std::snprintf(linhas[4], TAMANHO_LINHA, "TENT: %hu/%hhu",
                          limitar_uint16(wifi.tentativas_ultima_sessao),
                          wifi.politica.tentativas_por_rede);
            std::snprintf(
                linhas[5], TAMANHO_LINHA, "T:%huS LIM:%huS",
                limitar_uint16(wifi.politica.tempo_por_tentativa_ms / 1000u),
                limitar_uint16(wifi.politica.tempo_limite_sessao_ms / 1000u));
            std::snprintf(linhas[6], TAMANHO_LINHA, "ULTIMO: %.13s",
                          esp_err_to_name(wifi.ultimo_erro));
            copiar_linha(linhas[7], wifi.sessao_em_andamento
                                        ? "RADIO EM USO"
                                        : "RADIO SOB DEMANDA");
            return 8;
        case EstadoSinalizacao::OtaVerificando:
            copiar_linha(linhas[0], "OTA | VERIFICACAO");
            std::snprintf(linhas[1], TAMANHO_LINHA, "FASE: %.15s",
                          nome_fase_verificacao_ota(ota.estado));
            copiar_linha(linhas[2], wifi.conectado ? "WIFI: CONECTADO" :
                                                    "WIFI: PROCURANDO");
            std::snprintf(linhas[3], TAMANHO_LINHA, "REDE: %.15s",
                          wifi.rede_ativa[0] ? wifi.rede_ativa : "-");
            if (wifi.conectado) {
                std::snprintf(linhas[4], TAMANHO_LINHA, "SINAL:%hhdDBM CH:%hhu",
                              wifi.rssi_dbm, wifi.canal);
            } else {
                std::snprintf(linhas[4], TAMANHO_LINHA, "TENT: %hu/%hhu",
                              limitar_uint16(wifi.tentativas_ultima_sessao),
                              wifi.politica.tentativas_por_rede);
            }
            std::snprintf(
                linhas[5], TAMANHO_LINHA, "LIMITE: %hu SEG",
                limitar_uint16(wifi.politica.tempo_limite_sessao_ms / 1000u));
            std::snprintf(linhas[6], TAMANHO_LINHA, "FW: %.17s",
                          ota.versao_atual[0] ? ota.versao_atual : "-");
            std::snprintf(linhas[7], TAMANHO_LINHA, "VERIFICACAO: %hu",
                          limitar_uint16(ota.verificacoes));
            return 8;
        case EstadoSinalizacao::OtaAguardandoAutorizacao:
            copiar_linha(linhas[0], "OTA | DISPONIVEL");
            if (ota.versao_disponivel[0]) {
                std::snprintf(linhas[1], TAMANHO_LINHA, "MST:NOVA %.5s>%.5s",
                              ota.versao_atual, ota.versao_disponivel);
            } else {
                std::snprintf(linhas[1], TAMANHO_LINHA, "MST:OK %.13s",
                              ota.versao_atual);
            }
            montar_linha_versao_satelite(linhas[2], "EQP", ota.equipe);
            montar_linha_versao_satelite(linhas[3], "VIS", ota.visitantes);
            copiar_linha(linhas[5], "AGUARDA AUTORIZACAO");
            copiar_linha(linhas[6], "TERMINAL OU WEB");
            copiar_linha(linhas[7], "NAO DESLIGUE");
            return 8;
        case EstadoSinalizacao::OtaBaixando:
            if (ota.alvo_ativo == AlvoOta::Equipe ||
                ota.alvo_ativo == AlvoOta::Visitantes) {
                const SituacaoOtaSatelite& satelite =
                    ota.alvo_ativo == AlvoOta::Equipe ? ota.equipe : ota.visitantes;
                copiar_linha(linhas[0], "OTA SATELITE");
                std::snprintf(linhas[1], TAMANHO_LINHA, "ALVO: %s",
                              servico_ota_nome_alvo(ota.alvo_ativo));
                std::snprintf(linhas[2], TAMANHO_LINHA, "VERSAO: %.13s",
                              satelite.versao_disponivel);
                montar_progresso_ota(linhas[3], satelite.progresso);
                std::snprintf(linhas[4], TAMANHO_LINHA, "TAXA: %.2f MB/S",
                              satelite.progresso.taxa_bytes_por_segundo / (1024.0 * 1024.0));
                std::snprintf(linhas[5], TAMANHO_LINHA, "RESTANTE: %lu S",
                              static_cast<unsigned long>(satelite.progresso.tempo_restante_segundos));
                copiar_linha(linhas[6], "LINK UART: 1 MBPS");
                *progresso_decimos = static_cast<int>(satelite.progresso.percentual_decimos);
                return 7;
            } else {
                copiar_linha(linhas[0], "OTA MESTRE");
                std::snprintf(linhas[1], TAMANHO_LINHA, "VERSAO: %.13s",
                              ota.versao_disponivel[0] ? ota.versao_disponivel : "-");
                montar_progresso_ota(linhas[2], ota.progresso);
                std::snprintf(linhas[3], TAMANHO_LINHA, "TAXA: %.2f MB/S",
                              ota.progresso.taxa_bytes_por_segundo / (1024.0 * 1024.0));
                std::snprintf(linhas[4], TAMANHO_LINHA, "RESTANTE: %lu S",
                              static_cast<unsigned long>(ota.progresso.tempo_restante_segundos));
                copiar_linha(linhas[5], "NAO DESLIGUE");
                std::snprintf(linhas[6], TAMANHO_LINHA, "WIFI: %hhd DBM",
                              wifi.rssi_dbm);
                *progresso_decimos = static_cast<int>(ota.progresso.percentual_decimos);
                return 7;
            }
        case EstadoSinalizacao::OtaConcluido:
            copiar_linha(linhas[1], "OTA CONCLUIDA");
            std::snprintf(linhas[2], TAMANHO_LINHA, "ALVO: %.12s",
                          servico_ota_nome_alvo(ota.alvo_ativo));
            copiar_linha(linhas[4], "FIRMWARE VALIDADO");
            copiar_linha(linhas[6], "REINICIANDO...");
            return 7;
        case EstadoSinalizacao::OtaFalha:
            return montar_alerta(linhas, estado, saude, ota);
        case EstadoSinalizacao::Aviso:
            return montar_alerta(linhas, estado, saude, ota);
        case EstadoSinalizacao::Inicializando:
        case EstadoSinalizacao::Falha:
        case EstadoSinalizacao::Desligado:
            break;
    }
    return 1;
}

void registrar_atualizacao(EstadoSinalizacao estado, esp_err_t erro) {
    portENTER_CRITICAL(&trava_situacao);
    situacao.ultimo_erro = erro;
    situacao.tela_disponivel = erro == ESP_OK;
    if (erro == ESP_OK) {
        situacao.atualizacoes_ok++;
        situacao.ultimo_estado_exibido = estado;
    } else {
        situacao.falhas_atualizacao++;
    }
    portEXIT_CRITICAL(&trava_situacao);
}

void tarefa_painel(void*) {
    int64_t proxima_tentativa_ms = 0;
    MensagemPainelAtiva mensagem_ativa{};
    bool tela_estava_reservada = false;
    int64_t inicio_reserva_ms = 0;
    while (true) {
        const int64_t instante_ms = esp_timer_get_time() / 1000;
        if (instante_ms >= proxima_tentativa_ms) {
            detectar_conclusao_ota();
            const EstadoSinalizacao estado_sistema =
                servico_sinalizacao_obter_situacao().estado;
            const EstadoSinalizacao estado =
                selecionar_estado_exibicao(estado_sistema, instante_ms);
            const bool tela_reservada = estado_reserva_tela(estado_sistema);
            if (mensagem_ativa.ativa && tela_reservada &&
                !tela_estava_reservada) {
                inicio_reserva_ms = instante_ms;
            } else if (mensagem_ativa.ativa && !tela_reservada &&
                       tela_estava_reservada) {
                mensagem_ativa.expira_em_ms += instante_ms - inicio_reserva_ms;
            }
            tela_estava_reservada = tela_reservada;
            if (mensagem_ativa.ativa && !tela_reservada &&
                instante_ms >= mensagem_ativa.expira_em_ms) {
                mensagem_ativa = {};
            }
            if (!mensagem_ativa.ativa && fila_mensagens != nullptr &&
                !tela_reservada) {
                MensagemPainelLocal proxima{};
                if (xQueueReceive(fila_mensagens, &proxima, 0) == pdTRUE) {
                    mensagem_ativa.mensagem = proxima;
                    mensagem_ativa.expira_em_ms =
                        instante_ms + proxima.duracao_ms;
                    mensagem_ativa.ativa = true;
                }
            }
            LinhasPainel linhas{};
            int progresso_decimos = -1;
            const bool exibir_mensagem = mensagem_ativa.ativa &&
                                         !tela_reservada;
            const std::size_t quantidade = exibir_mensagem
                ? montar_mensagem(mensagem_ativa.mensagem, linhas)
                : montar_conteudo(estado, linhas, &progresso_decimos);
            const char* ponteiros[MAXIMO_LINHAS]{};
            for (std::size_t indice = 0; indice < quantidade; ++indice) {
                ponteiros[indice] = linhas[indice];
            }
            const esp_err_t erro = progresso_decimos >= 0
                ? tela_ssd1306_exibir_linhas_com_progresso(
                      ponteiros, quantidade,
                      static_cast<uint16_t>(progresso_decimos))
                : tela_ssd1306_exibir_linhas(ponteiros, quantidade);
            registrar_atualizacao(exibir_mensagem ? estado_sistema : estado,
                                   erro);
            if (erro != ESP_OK) {
                ESP_LOGE(ETIQUETA, "Falha ao atualizar a tela: %s; nova tentativa em %lu ms",
                         esp_err_to_name(erro),
                         static_cast<unsigned long>(INTERVALO_REPETICAO_ERRO_MS));
                proxima_tentativa_ms =
                    instante_ms + INTERVALO_REPETICAO_ERRO_MS;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(configuracao::INTERVALO_PAINEL_LOCAL_MS));
    }
}
}  // namespace

esp_err_t servico_painel_local_iniciar() {
    portENTER_CRITICAL(&trava_situacao);
    const bool ja_iniciado = situacao.iniciado;
    portEXIT_CRITICAL(&trava_situacao);
    if (ja_iniciado) return ESP_OK;

    const esp_err_t erro_tela = tela_ssd1306_iniciar();
    if (erro_tela != ESP_OK) {
        portENTER_CRITICAL(&trava_situacao);
        situacao.ultimo_erro = erro_tela;
        situacao.tela_disponivel = false;
        portEXIT_CRITICAL(&trava_situacao);
        return erro_tela;
    }

    portENTER_CRITICAL(&trava_situacao);
    situacao = {};
    situacao.iniciado = true;
    situacao.tela_disponivel = true;
    etapa_inicializacao = {};
    portEXIT_CRITICAL(&trava_situacao);
    fila_mensagens = xQueueCreate(CAPACIDADE_FILA_MENSAGENS,
                                  sizeof(MensagemPainelLocal));
    if (fila_mensagens == nullptr) {
        portENTER_CRITICAL(&trava_situacao);
        situacao.iniciado = false;
        situacao.ultimo_erro = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&trava_situacao);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tarefa_painel, "painel_local", 4096, nullptr, 2,
                    &tarefa_painel_handle) != pdPASS) {
        vQueueDelete(fila_mensagens);
        fila_mensagens = nullptr;
        portENTER_CRITICAL(&trava_situacao);
        situacao.iniciado = false;
        situacao.ultimo_erro = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&trava_situacao);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA, "Painel local SSD1306 iniciado");
    return ESP_OK;
}

esp_err_t servico_painel_local_atualizar_inicializacao(
    const char* etapa, uint8_t percentual, esp_err_t resultado) {
    if (etapa == nullptr || etapa[0] == '\0' || percentual > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    EtapaInicializacaoPainel nova_etapa{};
    std::snprintf(nova_etapa.descricao, sizeof(nova_etapa.descricao),
                  "%.21s", etapa);
    nova_etapa.percentual = percentual;
    nova_etapa.resultado = resultado;
    portENTER_CRITICAL(&trava_situacao);
    const bool painel_iniciado = situacao.iniciado;
    if (painel_iniciado) etapa_inicializacao = nova_etapa;
    portEXIT_CRITICAL(&trava_situacao);
    return painel_iniciado ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t servico_painel_local_notificar(TipoMensagemPainelLocal tipo,
                                         const char* titulo,
                                         const char* detalhe,
                                         uint32_t duracao_ms) {
    if (fila_mensagens == nullptr) return ESP_ERR_INVALID_STATE;
    if (titulo == nullptr || titulo[0] == '\0' || duracao_ms < 1000 ||
        duracao_ms > 30000) {
        return ESP_ERR_INVALID_ARG;
    }
    MensagemPainelLocal mensagem{};
    mensagem.tipo = tipo;
    std::snprintf(mensagem.titulo, sizeof(mensagem.titulo), "%.21s", titulo);
    std::snprintf(mensagem.detalhe, sizeof(mensagem.detalhe), "%.21s",
                  detalhe == nullptr ? "" : detalhe);
    mensagem.duracao_ms = duracao_ms;
    if (xQueueSend(fila_mensagens, &mensagem, 0) != pdTRUE) {
        ESP_LOGW(ETIQUETA,
                 "Fila de mensagens da tela cheia; aviso '%s' descartado",
                 mensagem.titulo);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
