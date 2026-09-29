#pragma once

#include <cstdint>

#include "esp_err.h"

enum class EstadoServicoOta : uint8_t {
    Desabilitado,
    Inicializando,
    AguardandoRede,
    Verificando,
    Atualizado,
    AguardandoAutorizacao,
    Baixando,
    Aplicado,
    Cancelado,
    Falha,
};

enum class AlvoOta : uint8_t {
    Nenhum,
    Mestre,
    Equipe,
    Visitantes,
};

enum class EstadoOtaSatelite : uint8_t {
    Desconhecido,
    Indisponivel,
    NaoSuportado,
    Atualizado,
    AguardandoAutorizacao,
    Transferindo,
    Reiniciando,
    Concluido,
    Cancelado,
    Falha,
};

struct ProgressoTransferenciaOta {
    uint32_t bytes_transferidos = 0;
    uint32_t tamanho_total = 0;
    uint32_t percentual_decimos = 0;
    uint32_t taxa_bytes_por_segundo = 0;
    uint32_t tempo_decorrido_ms = 0;
    uint32_t tempo_restante_segundos = 0;
};

struct SituacaoOtaSatelite {
    EstadoOtaSatelite estado = EstadoOtaSatelite::Desconhecido;
    char versao_atual[24] = {};
    char versao_disponivel[24] = {};
    ProgressoTransferenciaOta progresso;
    esp_err_t ultimo_erro = ESP_OK;
};

struct SituacaoOta {
    EstadoServicoOta estado = EstadoServicoOta::Desabilitado;
    char versao_atual[32] = {};
    char versao_disponivel[32] = {};
    uint32_t verificacoes = 0;
    uint32_t falhas = 0;
    uint32_t intervalo_verificacao_minutos = 0;
    esp_err_t ultimo_erro = ESP_OK;
    AlvoOta alvo_ativo = AlvoOta::Nenhum;
    ProgressoTransferenciaOta progresso;
    SituacaoOtaSatelite equipe;
    SituacaoOtaSatelite visitantes;
};

esp_err_t servico_ota_iniciar();
esp_err_t servico_ota_confirmar_firmware_em_execucao();
esp_err_t servico_ota_rejeitar_firmware_em_execucao();
esp_err_t servico_ota_solicitar_verificacao();
esp_err_t servico_ota_autorizar_atualizacao();
esp_err_t servico_ota_autorizar_atualizacao_satelite(AlvoOta alvo);
esp_err_t servico_ota_cancelar_atualizacao();
esp_err_t servico_ota_definir_intervalo_verificacao(uint32_t minutos);
SituacaoOta servico_ota_obter_situacao();
const char* servico_ota_nome_estado(EstadoServicoOta estado);
const char* servico_ota_nome_estado_satelite(EstadoOtaSatelite estado);
const char* servico_ota_nome_alvo(AlvoOta alvo);
