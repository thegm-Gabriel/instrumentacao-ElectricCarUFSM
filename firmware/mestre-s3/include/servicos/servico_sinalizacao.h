#pragma once

#include <cstdint>

#include "esp_err.h"

enum class EventoSinalizacao : uint8_t {
    Inicializacao,
    SistemaPronto,
    OperacaoConfirmada,
    AvisoTemporario,
    FalhaCritica,
    LimparFalhaCritica,
};

enum class EstadoSinalizacao : uint8_t {
    Desligado,
    Inicializando,
    Saudavel,
    WifiAtivo,
    OtaVerificando,
    OtaAguardandoAutorizacao,
    OtaBaixando,
    OtaConcluido,
    OtaFalha,
    Aviso,
    Falha,
};

struct SituacaoSinalizacao {
    bool iniciado = false;
    bool sistema_pronto = false;
    bool falha_critica_travada = false;
    bool melodia_em_execucao = false;
    EstadoSinalizacao estado = EstadoSinalizacao::Desligado;
    uint32_t eventos_recebidos = 0;
    uint32_t eventos_descartados = 0;
    uint32_t falhas_hardware = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

// Inicia uma tarefa leve: LEDs e melodias são atualizados sem esperas bloqueantes.
esp_err_t servico_sinalizacao_iniciar();
esp_err_t servico_sinalizacao_notificar(EventoSinalizacao evento);
SituacaoSinalizacao servico_sinalizacao_obter_situacao();
const char* servico_sinalizacao_nome_estado(EstadoSinalizacao estado);
