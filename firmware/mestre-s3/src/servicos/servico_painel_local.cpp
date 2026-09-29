#include "servicos/servico_painel_local.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dispositivos/tela_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao_placa.h"
#include "servicos/servico_diagnostico.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_telemetria.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "painel_local";
constexpr std::size_t MAXIMO_LINHAS = 8;
constexpr std::size_t TAMANHO_LINHA = 22;
constexpr uint32_t INTERVALO_REPETICAO_ERRO_MS = 5000;
constexpr uint32_t INTERVALO_PAGINA_SAUDAVEL_MS = 6000;

portMUX_TYPE trava_situacao = portMUX_INITIALIZER_UNLOCKED;
SituacaoPainelLocal situacao{};
TaskHandle_t tarefa_painel_handle = nullptr;

using LinhasPainel = char[MAXIMO_LINHAS][TAMANHO_LINHA];

void copiar_linha(char* destino, const char* origem) {
    if (destino == nullptr) return;
    std::snprintf(destino, TAMANHO_LINHA, "%.21s", origem == nullptr ? "" : origem);
}

void montar_estado_links(char* destino, const ResumoSaudeSistema& saude) {
    std::snprintf(destino, TAMANHO_LINHA, "EQP:%s  VIS:%s",
                  saude.equipe_respondendo ? "OK" : "--",
                  saude.visitantes_respondendo ? "OK" : "--");
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
        std::snprintf(linhas[5], TAMANHO_LINHA, "HEAP: %hu KB",
                      limitar_uint16(saude.heap_livre / 1024u));
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
        copiar_linha(linhas[2], "INICIALIZANDO...");
        copiar_linha(linhas[4], "AGUARDE");
        return 5;
    }
    if (estado == EstadoSinalizacao::Falha) {
        copiar_linha(linhas[2], "FALHA CRITICA");
        copiar_linha(linhas[4], "CONSULTE O TERMINAL");
        copiar_linha(linhas[6], "SISTEMA INTERROMPIDO");
        return 7;
    }
    if (estado == EstadoSinalizacao::Desligado) {
        copiar_linha(linhas[3], "PAINEL EM ESPERA");
        return 4;
    }

    const SituacaoOta ota = servico_ota_obter_situacao();
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    const ResumoSaudeSistema saude = servico_diagnostico_obter_resumo();
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
            copiar_linha(linhas[0], "OTA | FALHA");
            copiar_linha(linhas[1], "ULTIMA OPERACAO");
            std::snprintf(linhas[2], TAMANHO_LINHA, "ERRO: %.14s",
                          esp_err_to_name(ota.ultimo_erro));
            copiar_linha(linhas[3], wifi.radio_ativo ? "WIFI: ATIVO" :
                                                  "WIFI: REPOUSO");
            std::snprintf(linhas[4], TAMANHO_LINHA, "REDE: %.15s",
                          wifi.rede_ativa[0] ? wifi.rede_ativa : "-");
            std::snprintf(linhas[5], TAMANHO_LINHA, "TENTATIVAS: %hu",
                          limitar_uint16(wifi.tentativas_ultima_sessao));
            copiar_linha(linhas[7], "CONSULTE O TERMINAL");
            return 8;
        case EstadoSinalizacao::Aviso:
            copiar_linha(linhas[2], "ATENCAO");
            montar_estado_links(linhas[3], saude);
            copiar_linha(linhas[5], "CONSULTE O TERMINAL");
            return 6;
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
    while (true) {
        const int64_t instante_ms = esp_timer_get_time() / 1000;
        if (instante_ms >= proxima_tentativa_ms) {
            const EstadoSinalizacao estado =
                servico_sinalizacao_obter_situacao().estado;
            LinhasPainel linhas{};
            int progresso_decimos = -1;
            const std::size_t quantidade = montar_conteudo(
                estado, linhas, &progresso_decimos);
            const char* ponteiros[MAXIMO_LINHAS]{};
            for (std::size_t indice = 0; indice < quantidade; ++indice) {
                ponteiros[indice] = linhas[indice];
            }
            const esp_err_t erro = progresso_decimos >= 0
                ? tela_ssd1306_exibir_linhas_com_progresso(
                      ponteiros, quantidade,
                      static_cast<uint16_t>(progresso_decimos))
                : tela_ssd1306_exibir_linhas(ponteiros, quantidade);
            registrar_atualizacao(estado, erro);
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
    portEXIT_CRITICAL(&trava_situacao);
    if (xTaskCreate(tarefa_painel, "painel_local", 4096, nullptr, 2,
                    &tarefa_painel_handle) != pdPASS) {
        portENTER_CRITICAL(&trava_situacao);
        situacao.iniciado = false;
        situacao.ultimo_erro = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&trava_situacao);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA, "Painel local SSD1306 iniciado");
    return ESP_OK;
}

SituacaoPainelLocal servico_painel_local_obter_situacao() {
    portENTER_CRITICAL(&trava_situacao);
    const SituacaoPainelLocal copia = situacao;
    portEXIT_CRITICAL(&trava_situacao);
    return copia;
}
