#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "gerenciadores/gerenciador_uart.h"
#include "protocolo_comandos.h"

struct EstatisticasServicoComandos {
    uint32_t solicitacoes_recebidas = 0;
    uint32_t respostas_enviadas = 0;
    uint32_t solicitacoes_enviadas = 0;
    uint32_t respostas_recebidas = 0;
    uint32_t repeticoes = 0;
    uint32_t timeouts = 0;
    uint32_t quadros_invalidos = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

using AcaoSolicitarResumo = void (*)();

// A coordenação da aplicação informa a ação sem acoplar comandos ao terminal.
void servico_comandos_definir_acao_resumo(AcaoSolicitarResumo acao);
esp_err_t servico_comandos_iniciar();
void servico_comandos_processar_quadro(DestinoUart origem,
                                       const uint8_t* quadro, size_t tamanho);
esp_err_t servico_comandos_solicitar(
    DestinoUart destino, codigo_comando_t comando, const void* carga,
    uint16_t tamanho_carga, void* resposta, size_t capacidade_resposta,
    uint16_t* tamanho_resposta, codigo_resposta_comando_t* resultado,
    TickType_t tempo_limite = pdMS_TO_TICKS(1000));
EstatisticasServicoComandos servico_comandos_obter_estatisticas();
