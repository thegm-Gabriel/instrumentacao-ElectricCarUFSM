#include "gerenciadores/gerenciador_uart.h"

#include <climits>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nucleo/configuracao_placa.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_uart";

struct CanalUart {
    DestinoUart destino;
    const char* nome;
    uart_port_t porta;
    gpio_num_t pino_tx;
    gpio_num_t pino_rx;
    SemaphoreHandle_t mutex_tx;
    SemaphoreHandle_t mutex_rx;
    QueueHandle_t fila_eventos;
    EstatisticasUart estatisticas;
};

CanalUart canais[] = {
    {DestinoUart::Equipe, "equipe", configuracao::UART_EQUIPE,
     configuracao::PINO_UART_EQUIPE_TX, configuracao::PINO_UART_EQUIPE_RX,
     nullptr, nullptr, nullptr, {}},
    {DestinoUart::Visitantes, "visitantes", configuracao::UART_VISITANTES,
     configuracao::PINO_UART_VISITANTES_TX, configuracao::PINO_UART_VISITANTES_RX,
     nullptr, nullptr, nullptr, {}},
};

portMUX_TYPE trava_estatisticas = portMUX_INITIALIZER_UNLOCKED;

CanalUart* encontrar_canal(DestinoUart destino) {
    for (auto& canal : canais) {
        if (canal.destino == destino) return &canal;
    }
    return nullptr;
}

void registrar_erro(CanalUart& canal, esp_err_t erro, bool transmissao) {
    portENTER_CRITICAL(&trava_estatisticas);
    if (transmissao) {
        canal.estatisticas.erros_envio++;
        canal.estatisticas.ultimo_erro_envio = erro;
    } else {
        canal.estatisticas.erros_recepcao++;
        canal.estatisticas.ultimo_erro_recepcao = erro;
    }
    portEXIT_CRITICAL(&trava_estatisticas);
}

void tarefa_eventos_uart(void* argumento) {
    auto& canal = *static_cast<CanalUart*>(argumento);
    uart_event_t evento{};
    while (true) {
        if (xQueueReceive(canal.fila_eventos, &evento, portMAX_DELAY) != pdTRUE) continue;
        if (evento.type == UART_DATA) continue;

        portENTER_CRITICAL(&trava_estatisticas);
        switch (evento.type) {
            case UART_FIFO_OVF: canal.estatisticas.estouros_fifo++; break;
            case UART_BUFFER_FULL: canal.estatisticas.buffers_cheios++; break;
            case UART_FRAME_ERR: canal.estatisticas.erros_quadro++; break;
            case UART_PARITY_ERR: canal.estatisticas.erros_paridade++; break;
            case UART_BREAK: canal.estatisticas.sinais_break++; break;
            default: break;
        }
        canal.estatisticas.erros_recepcao++;
        canal.estatisticas.ultimo_erro_recepcao = ESP_FAIL;
        portEXIT_CRITICAL(&trava_estatisticas);

        if (evento.type == UART_FIFO_OVF || evento.type == UART_BUFFER_FULL) {
            ESP_LOGE(ETIQUETA, "%s: buffer de recepcao saturado", canal.nome);
            if (xSemaphoreTake(canal.mutex_rx, pdMS_TO_TICKS(10)) == pdTRUE) {
                uart_flush_input(canal.porta);
                xQueueReset(canal.fila_eventos);
                xSemaphoreGive(canal.mutex_rx);
            }
        } else if (evento.type == UART_FRAME_ERR) {
            ESP_LOGE(ETIQUETA, "%s: erro de quadro; verifique baud rate e sinal", canal.nome);
        } else if (evento.type == UART_PARITY_ERR) {
            ESP_LOGE(ETIQUETA, "%s: erro de paridade", canal.nome);
        } else if (evento.type == UART_BREAK) {
            ESP_LOGW(ETIQUETA, "%s: sinal de break detectado", canal.nome);
        } else {
            ESP_LOGW(ETIQUETA, "%s: evento UART desconhecido (%d)", canal.nome, evento.type);
        }
    }
}

esp_err_t configurar_canal(CanalUart& canal) {
    if (canal.estatisticas.inicializado) return ESP_OK;
    if (canal.mutex_tx == nullptr) canal.mutex_tx = xSemaphoreCreateMutex();
    if (canal.mutex_rx == nullptr) canal.mutex_rx = xSemaphoreCreateMutex();
    if (canal.mutex_tx == nullptr || canal.mutex_rx == nullptr) {
        ESP_LOGE(ETIQUETA, "%s: memoria insuficiente para mutexes", canal.nome);
        registrar_erro(canal, ESP_ERR_NO_MEM, true);
        return ESP_ERR_NO_MEM;
    }

    uart_config_t parametros{};
    parametros.baud_rate = configuracao::VELOCIDADE_UART;
    parametros.data_bits = UART_DATA_8_BITS;
    parametros.parity = UART_PARITY_DISABLE;
    parametros.stop_bits = UART_STOP_BITS_1;
    parametros.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    parametros.source_clk = UART_SCLK_DEFAULT;

    esp_err_t erro = uart_driver_install(canal.porta, configuracao::TAMANHO_BUFFER_UART_RX,
                                         configuracao::TAMANHO_BUFFER_UART_TX, 20,
                                         &canal.fila_eventos, 0);
    if (erro == ESP_OK) erro = uart_param_config(canal.porta, &parametros);
    if (erro == ESP_OK) {
        erro = uart_set_pin(canal.porta, canal.pino_tx, canal.pino_rx,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (erro == ESP_OK &&
        xTaskCreate(tarefa_eventos_uart, canal.nome, 3072, &canal, 6, nullptr) != pdPASS) {
        erro = ESP_ERR_NO_MEM;
    }
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "%s: inicializacao falhou: %s", canal.nome,
                 esp_err_to_name(erro));
        uart_driver_delete(canal.porta);
        canal.fila_eventos = nullptr;
        registrar_erro(canal, erro, true);
        return erro;
    }

    portENTER_CRITICAL(&trava_estatisticas);
    canal.estatisticas.inicializado = true;
    portEXIT_CRITICAL(&trava_estatisticas);
    ESP_LOGI(ETIQUETA, "%s pronta: UART %d, TX=%d, RX=%d, %d baud 8N1",
             canal.nome, canal.porta, canal.pino_tx, canal.pino_rx,
             configuracao::VELOCIDADE_UART);
    return ESP_OK;
}

esp_err_t enviar_canal(CanalUart& canal, const void* dados, size_t tamanho,
                       TickType_t tempo_limite) {
    if (!canal.estatisticas.inicializado) {
        ESP_LOGE(ETIQUETA, "%s: envio antes da inicializacao", canal.nome);
        registrar_erro(canal, ESP_ERR_INVALID_STATE, true);
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(canal.mutex_tx, tempo_limite) != pdTRUE) {
        ESP_LOGE(ETIQUETA, "%s: timeout aguardando transmissor", canal.nome);
        registrar_erro(canal, ESP_ERR_TIMEOUT, true);
        return ESP_ERR_TIMEOUT;
    }

    int escritos = uart_write_bytes(canal.porta, dados, tamanho);
    esp_err_t resultado = ESP_OK;
    if (escritos != static_cast<int>(tamanho)) {
        resultado = ESP_FAIL;
        ESP_LOGE(ETIQUETA, "%s: UART aceitou %d de %u bytes", canal.nome, escritos,
                 static_cast<unsigned>(tamanho));
    } else {
        resultado = uart_wait_tx_done(canal.porta, tempo_limite);
        if (resultado != ESP_OK) {
            ESP_LOGE(ETIQUETA, "%s: transmissao incompleta: %s", canal.nome,
                     esp_err_to_name(resultado));
        }
    }
    xSemaphoreGive(canal.mutex_tx);

    portENTER_CRITICAL(&trava_estatisticas);
    if (resultado == ESP_OK) {
        canal.estatisticas.envios_ok++;
        canal.estatisticas.bytes_enviados += tamanho;
    } else {
        canal.estatisticas.erros_envio++;
        canal.estatisticas.ultimo_erro_envio = resultado;
    }
    portEXIT_CRITICAL(&trava_estatisticas);
    return resultado;
}
}  // namespace

esp_err_t gerenciador_uart_iniciar() {
    esp_err_t primeiro_erro = ESP_OK;
    for (auto& canal : canais) {
        esp_err_t erro = configurar_canal(canal);
        if (erro != ESP_OK && primeiro_erro == ESP_OK) primeiro_erro = erro;
    }
    if (primeiro_erro == ESP_OK) ESP_LOGI(ETIQUETA, "Interfaces UART prontas");
    return primeiro_erro;
}

esp_err_t gerenciador_uart_enviar(DestinoUart destino, const void* dados, size_t tamanho,
                                  TickType_t tempo_limite) {
    if (dados == nullptr || tamanho == 0 || tamanho > INT_MAX) {
        ESP_LOGE(ETIQUETA, "Envio rejeitado: argumento invalido");
        return ESP_ERR_INVALID_ARG;
    }
    if (destino == DestinoUart::Todos) {
        esp_err_t equipe = enviar_canal(canais[0], dados, tamanho, tempo_limite);
        esp_err_t visitantes = enviar_canal(canais[1], dados, tamanho, tempo_limite);
        return equipe != ESP_OK ? equipe : visitantes;
    }
    CanalUart* canal = encontrar_canal(destino);
    if (canal == nullptr) {
        ESP_LOGE(ETIQUETA, "Destino UART invalido");
        return ESP_ERR_INVALID_ARG;
    }
    return enviar_canal(*canal, dados, tamanho, tempo_limite);
}

int gerenciador_uart_receber(DestinoUart origem, void* buffer, size_t capacidade,
                             TickType_t tempo_limite) {
    CanalUart* canal = encontrar_canal(origem);
    if (canal == nullptr || buffer == nullptr || capacidade == 0 || capacidade > INT_MAX) {
        ESP_LOGE(ETIQUETA, "Recepcao rejeitada: argumento invalido");
        return -1;
    }
    if (!canal->estatisticas.inicializado) {
        ESP_LOGE(ETIQUETA, "%s: recepcao antes da inicializacao", canal->nome);
        registrar_erro(*canal, ESP_ERR_INVALID_STATE, false);
        return -1;
    }
    if (xSemaphoreTake(canal->mutex_rx, tempo_limite) != pdTRUE) {
        registrar_erro(*canal, ESP_ERR_TIMEOUT, false);
        return -1;
    }
    int recebidos = uart_read_bytes(canal->porta, buffer, capacidade, tempo_limite);
    xSemaphoreGive(canal->mutex_rx);

    portENTER_CRITICAL(&trava_estatisticas);
    if (recebidos >= 0) {
        if (recebidos > 0) canal->estatisticas.recepcoes_ok++;
        canal->estatisticas.bytes_recebidos += recebidos;
    } else {
        canal->estatisticas.erros_recepcao++;
        canal->estatisticas.ultimo_erro_recepcao = ESP_FAIL;
    }
    portEXIT_CRITICAL(&trava_estatisticas);
    if (recebidos < 0) ESP_LOGE(ETIQUETA, "%s: leitura falhou", canal->nome);
    return recebidos;
}

esp_err_t gerenciador_uart_limpar_recepcao(DestinoUart origem) {
    CanalUart* canal = encontrar_canal(origem);
    if (canal == nullptr || !canal->estatisticas.inicializado) return ESP_ERR_INVALID_STATE;
    esp_err_t erro = uart_flush_input(canal->porta);
    if (erro != ESP_OK) registrar_erro(*canal, erro, false);
    return erro;
}

EstatisticasUart gerenciador_uart_obter_estatisticas(DestinoUart destino) {
    EstatisticasUart copia{};
    CanalUart* canal = encontrar_canal(destino);
    if (canal == nullptr) return copia;
    portENTER_CRITICAL(&trava_estatisticas);
    copia = canal->estatisticas;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}

const char* gerenciador_uart_nome(DestinoUart destino) {
    if (destino == DestinoUart::Equipe) return "equipe";
    if (destino == DestinoUart::Visitantes) return "visitantes";
    if (destino == DestinoUart::Todos) return "todos";
    return "desconhecido";
}
