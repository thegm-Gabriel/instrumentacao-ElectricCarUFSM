#include "servicos/servico_enlace_satelites.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_ota_satelites.h"
#include "gerenciadores/gerenciador_uart.h"
#include "nucleo/configuracao_placa.h"
#include "protocolo_enlace_uart.h"
#include "protocolo_telemetria.h"
#include "servicos/servico_comandos.h"
#include "servicos/servico_supervisao.h"
#include "servicos/servico_telemetria.h"

namespace {
constexpr char ETIQUETA[] = "enlace_satelites";
constexpr uint32_t MARGEM_SUPERVISAO_UART_MS = 5000;

struct CanalEnlace {
    SateliteTelemetria satelite;
    DestinoUart origem;
    const char* nome;
    TaskHandle_t tarefa;
    parser_enlace_uart_t parser;
    uint32_t quadros_invalidos;
};

CanalEnlace canais[] = {
    {SateliteTelemetria::Equipe, DestinoUart::Equipe, "equipe", nullptr, {}, 0},
    {SateliteTelemetria::Visitantes, DestinoUart::Visitantes, "visitantes", nullptr, {}, 0},
};
bool iniciado = false;

ModuloSupervisionado modulo_supervisionado(DestinoUart destino) {
    return destino == DestinoUart::Equipe ? ModuloSupervisionado::UartEquipe
                                          : ModuloSupervisionado::UartVisitantes;
}

esp_err_t recuperar_uart_equipe() {
    return gerenciador_uart_recuperar(DestinoUart::Equipe);
}

esp_err_t recuperar_uart_visitantes() {
    return gerenciador_uart_recuperar(DestinoUart::Visitantes);
}

void tarefa_recepcao(void* argumento) {
    auto& canal = *static_cast<CanalEnlace*>(argumento);
    uint8_t lote[64];
    while (true) {
        const int recebidos = gerenciador_uart_receber(
            canal.origem, lote, sizeof(lote), pdMS_TO_TICKS(100));
        servico_supervisao_alimentar(modulo_supervisionado(canal.origem));
        if (recebidos < 0) {
            // Evita laço ocupado se o driver entrar temporariamente em erro.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (recebidos == 0) continue;
        for (int indice = 0; indice < recebidos; ++indice) {
            const resultado_parser_enlace_uart_t resultado =
                protocolo_enlace_uart_processar_byte(&canal.parser, lote[indice]);
            if (resultado == ENLACE_UART_TELEMETRIA_COMPLETA) {
                confirmacao_telemetria_t confirmacao{};
                std::memcpy(&confirmacao, canal.parser.quadro, sizeof(confirmacao));
                servico_telemetria_receber_confirmacao(canal.satelite, confirmacao);
            } else if (resultado == ENLACE_UART_OTA_COMPLETA) {
                gerenciador_ota_satelites_registrar_resposta(
                    canal.origem, canal.parser.quadro, canal.parser.ultimo_tamanho);
            } else if (resultado == ENLACE_UART_COMANDO_COMPLETO) {
                servico_comandos_processar_quadro(
                    canal.origem, canal.parser.quadro, canal.parser.ultimo_tamanho);
            } else if (resultado == ENLACE_UART_QUADRO_INVALIDO) {
                canal.quadros_invalidos++;
                if (canal.quadros_invalidos == 1 ||
                    canal.quadros_invalidos % 100 == 0) {
                    ESP_LOGW(ETIQUETA, "%s: quadro de retorno inválido descartado (%lu)",
                             canal.nome,
                             static_cast<unsigned long>(canal.quadros_invalidos));
                }
            }
        }
    }
}
}  // namespace

esp_err_t servico_enlace_satelites_iniciar() {
    if (iniciado) return ESP_OK;
    const esp_err_t erro_uart = gerenciador_uart_iniciar();
    if (erro_uart != ESP_OK) return erro_uart;
    const esp_err_t erro_ota = gerenciador_ota_satelites_iniciar();
    if (erro_ota != ESP_OK) return erro_ota;
    const esp_err_t erro_comandos = servico_comandos_iniciar();
    if (erro_comandos != ESP_OK) return erro_comandos;
    for (auto& canal : canais) {
        if (canal.tarefa != nullptr) continue;
        protocolo_enlace_uart_inicializar(
            &canal.parser, TELEMETRIA_INICIO_CONFIRMACAO_2,
            TAMANHO_CONFIRMACAO_TELEMETRIA);
        const char* nome_tarefa = canal.origem == DestinoUart::Equipe
                                      ? "enlace_equipe" : "enlace_visitantes";
        if (xTaskCreate(tarefa_recepcao, nome_tarefa, 6144, &canal, 6,
                        &canal.tarefa) != pdPASS) {
            ESP_LOGE(ETIQUETA, "Não foi possível iniciar o enlace de %s", canal.nome);
            return ESP_ERR_NO_MEM;
        }
    }
    for (auto& canal : canais) {
        const ModuloSupervisionado modulo = modulo_supervisionado(canal.origem);
        const AcaoRecuperacaoModulo recuperar =
            canal.origem == DestinoUart::Equipe ? recuperar_uart_equipe
                                                : recuperar_uart_visitantes;
        const esp_err_t erro = servico_supervisao_registrar(
            modulo, canal.origem == DestinoUart::Equipe ? "enlace UART equipe"
                                                        : "enlace UART visitantes",
            configuracao::INTERVALO_TELEMETRIA_OTA_MAXIMO_MS +
                MARGEM_SUPERVISAO_UART_MS,
            recuperar);
        if (erro != ESP_OK) return erro;
        servico_supervisao_associar_tarefa(modulo, canal.tarefa);
        servico_supervisao_alimentar(modulo);
    }
    iniciado = true;
    ESP_LOGI(ETIQUETA,
             "Enlace dos satélites pronto e supervisionado: telemetria, comandos e OTA");
    return ESP_OK;
}
