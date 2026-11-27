// Declara consultas e ações remotas transportadas pela UART.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "nucleo/destino_enlace.h"
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

using DespachoComando = codigo_resposta_comando_t (*)(
    uint16_t comando, DestinoUart origem, const uint8_t* carga,
    uint16_t tamanho_carga, uint8_t* resposta, uint16_t* tamanho_resposta,
    int64_t recebido_em_us);

// Configurado durante a partida, antes de iniciar a recepção dos enlaces.
void servico_comandos_definir_despacho(DespachoComando despacho);

// Adaptador que a aplicação usa para ligar os comandos aos serviços do mestre.
codigo_resposta_comando_t comandos_mestre_despachar(
    uint16_t comando, DestinoUart origem, const uint8_t* carga,
    uint16_t tamanho_carga, uint8_t* resposta, uint16_t* tamanho_resposta,
    int64_t recebido_em_us);

esp_err_t servico_comandos_iniciar();
esp_err_t servico_comandos_encerrar();
void servico_comandos_processar_quadro(DestinoUart origem,
                                       const uint8_t* quadro, size_t tamanho);
esp_err_t servico_comandos_solicitar(
    DestinoUart destino, codigo_comando_t comando, const void* carga,
    uint16_t tamanho_carga, void* resposta, size_t capacidade_resposta,
    uint16_t* tamanho_resposta, codigo_resposta_comando_t* resultado,
    TickType_t tempo_limite = pdMS_TO_TICKS(1000));
EstatisticasServicoComandos servico_comandos_obter_estatisticas();
