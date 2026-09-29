#include "gerenciadores/gerenciador_i2c.h"

#include <climits>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "nucleo/configuracao_placa.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_i2c";
SemaphoreHandle_t mutex_barramento = nullptr;
portMUX_TYPE trava_estatisticas = portMUX_INITIALIZER_UNLOCKED;
EstatisticasI2c estatisticas{};

bool argumentos_validos(uint8_t endereco, const void* dados, size_t tamanho) {
    return endereco <= 0x7F && dados != nullptr && tamanho > 0 && tamanho <= INT_MAX;
}

void registrar_resultado(esp_err_t resultado, size_t escritos, size_t lidos) {
    portENTER_CRITICAL(&trava_estatisticas);
    if (resultado == ESP_OK) {
        estatisticas.operacoes_ok++;
        estatisticas.bytes_escritos += escritos;
        estatisticas.bytes_lidos += lidos;
        estatisticas.ultimo_erro = ESP_OK;
    } else {
        estatisticas.erros++;
        estatisticas.ultimo_erro = resultado;
    }
    portEXIT_CRITICAL(&trava_estatisticas);
}

void registrar_sondagem(bool respondeu) {
    portENTER_CRITICAL(&trava_estatisticas);
    if (respondeu) {
        estatisticas.sondagens_ok++;
    } else {
        estatisticas.enderecos_ausentes++;
    }
    portEXIT_CRITICAL(&trava_estatisticas);
}
}  // namespace

esp_err_t gerenciador_i2c_iniciar() {
    if (estatisticas.inicializado) return ESP_OK;
    if (mutex_barramento == nullptr) mutex_barramento = xSemaphoreCreateMutex();
    if (mutex_barramento == nullptr) return ESP_ERR_NO_MEM;

    i2c_config_t parametros{};
    parametros.mode = I2C_MODE_MASTER;
    parametros.sda_io_num = configuracao::PINO_I2C_SDA;
    parametros.scl_io_num = configuracao::PINO_I2C_SCL;
    parametros.sda_pullup_en = GPIO_PULLUP_ENABLE;
    parametros.scl_pullup_en = GPIO_PULLUP_ENABLE;
    parametros.master.clk_speed = configuracao::FREQUENCIA_I2C_HZ;

    esp_err_t erro = i2c_param_config(configuracao::PORTA_I2C, &parametros);
    if (erro == ESP_OK) erro = i2c_driver_install(configuracao::PORTA_I2C, I2C_MODE_MASTER, 0, 0, 0);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Inicializacao falhou: %s", esp_err_to_name(erro));
        registrar_resultado(erro, 0, 0);
        return erro;
    }

    portENTER_CRITICAL(&trava_estatisticas);
    estatisticas.inicializado = true;
    portEXIT_CRITICAL(&trava_estatisticas);
    ESP_LOGI(ETIQUETA, "I2C pronto: porta=%d SDA=%d SCL=%d frequencia=%lu Hz",
             configuracao::PORTA_I2C, configuracao::PINO_I2C_SDA,
             configuracao::PINO_I2C_SCL,
             static_cast<unsigned long>(configuracao::FREQUENCIA_I2C_HZ));
    return ESP_OK;
}

esp_err_t gerenciador_i2c_sondar(uint8_t endereco, TickType_t tempo_limite) {
    if (endereco > 0x7F || endereco == 0) return ESP_ERR_INVALID_ARG;
    if (!estatisticas.inicializado || mutex_barramento == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex_barramento, tempo_limite) != pdTRUE) {
        registrar_resultado(ESP_ERR_TIMEOUT, 0, 0);
        return ESP_ERR_TIMEOUT;
    }

    i2c_cmd_handle_t comandos = i2c_cmd_link_create();
    if (comandos == nullptr) {
        xSemaphoreGive(mutex_barramento);
        registrar_resultado(ESP_ERR_NO_MEM, 0, 0);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t erro = i2c_master_start(comandos);
    if (erro == ESP_OK) {
        erro = i2c_master_write_byte(
            comandos, static_cast<uint8_t>((endereco << 1) | I2C_MASTER_WRITE), true);
    }
    if (erro == ESP_OK) erro = i2c_master_stop(comandos);
    if (erro == ESP_OK) {
        erro = i2c_master_cmd_begin(configuracao::PORTA_I2C, comandos,
                                    tempo_limite);
    }
    i2c_cmd_link_delete(comandos);
    xSemaphoreGive(mutex_barramento);

    registrar_sondagem(erro == ESP_OK);
    return erro;
}

esp_err_t gerenciador_i2c_escrever(uint8_t endereco, const void* dados, size_t tamanho,
                                   TickType_t tempo_limite) {
    if (!argumentos_validos(endereco, dados, tamanho)) {
        ESP_LOGE(ETIQUETA, "Escrita rejeitada: argumento invalido");
        return ESP_ERR_INVALID_ARG;
    }
    if (!estatisticas.inicializado) {
        ESP_LOGE(ETIQUETA, "Escrita solicitada antes da inicializacao");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex_barramento, tempo_limite) != pdTRUE) {
        registrar_resultado(ESP_ERR_TIMEOUT, 0, 0);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t erro = i2c_master_write_to_device(
        configuracao::PORTA_I2C, endereco, static_cast<const uint8_t*>(dados), tamanho,
        tempo_limite);
    xSemaphoreGive(mutex_barramento);
    registrar_resultado(erro, erro == ESP_OK ? tamanho : 0, 0);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao escrever no endereco 0x%02X: %s", endereco,
                 esp_err_to_name(erro));
    }
    return erro;
}

esp_err_t gerenciador_i2c_escrever_ler(uint8_t endereco, const void* comando,
                                       size_t tamanho_comando, void* resposta,
                                       size_t tamanho_resposta, TickType_t tempo_limite) {
    if (!argumentos_validos(endereco, comando, tamanho_comando) || resposta == nullptr ||
        tamanho_resposta == 0 || tamanho_resposta > INT_MAX) {
        ESP_LOGE(ETIQUETA, "Leitura rejeitada: argumento invalido");
        return ESP_ERR_INVALID_ARG;
    }
    if (!estatisticas.inicializado) {
        ESP_LOGE(ETIQUETA, "Leitura solicitada antes da inicializacao");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex_barramento, tempo_limite) != pdTRUE) {
        registrar_resultado(ESP_ERR_TIMEOUT, 0, 0);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t erro = i2c_master_write_read_device(
        configuracao::PORTA_I2C, endereco, static_cast<const uint8_t*>(comando),
        tamanho_comando, static_cast<uint8_t*>(resposta), tamanho_resposta, tempo_limite);
    xSemaphoreGive(mutex_barramento);
    registrar_resultado(erro, erro == ESP_OK ? tamanho_comando : 0,
                        erro == ESP_OK ? tamanho_resposta : 0);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao ler o endereco 0x%02X: %s", endereco,
                 esp_err_to_name(erro));
    }
    return erro;
}

EstatisticasI2c gerenciador_i2c_obter_estatisticas() {
    portENTER_CRITICAL(&trava_estatisticas);
    EstatisticasI2c copia = estatisticas;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}
