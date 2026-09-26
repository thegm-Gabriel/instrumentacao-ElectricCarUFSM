#include "dispositivos/mpu6050.h"

#include "gerenciadores/gerenciador_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nucleo/configuracao_placa.h"

namespace {
constexpr char ETIQUETA[] = "mpu6050";
constexpr uint8_t REGISTRO_GERENCIAMENTO_ENERGIA = 0x6B;
constexpr uint8_t REGISTRO_DADOS_ACELERACAO = 0x3B;
bool inicializado = false;

int16_t juntar_bytes(uint8_t alto, uint8_t baixo) {
    return static_cast<int16_t>((static_cast<uint16_t>(alto) << 8) | baixo);
}
}  // namespace

esp_err_t mpu6050_iniciar() {
    if (inicializado) return ESP_OK;
    esp_err_t erro = gerenciador_i2c_iniciar();
    if (erro != ESP_OK) return erro;

    const uint8_t comando[] = {REGISTRO_GERENCIAMENTO_ENERGIA, 0x00};
    erro = gerenciador_i2c_escrever(configuracao::ENDERECO_MPU6050, comando,
                                    sizeof(comando),
                                    pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "MPU6050 nao respondeu durante a inicializacao: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    inicializado = true;
    ESP_LOGI(ETIQUETA, "MPU6050 pronto no endereco 0x%02X",
             configuracao::ENDERECO_MPU6050);
    return ESP_OK;
}

esp_err_t mpu6050_ler(LeituraMpu6050* leitura) {
    if (leitura == nullptr) return ESP_ERR_INVALID_ARG;
    if (!inicializado) return ESP_ERR_INVALID_STATE;

    uint8_t dados[14]{};
    const uint8_t registro = REGISTRO_DADOS_ACELERACAO;
    esp_err_t erro = gerenciador_i2c_escrever_ler(
        configuracao::ENDERECO_MPU6050, &registro, sizeof(registro), dados, sizeof(dados),
        pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
    if (erro != ESP_OK) return erro;

    LeituraMpu6050 nova{};
    nova.aceleracao_x = juntar_bytes(dados[0], dados[1]);
    nova.aceleracao_y = juntar_bytes(dados[2], dados[3]);
    nova.aceleracao_z = juntar_bytes(dados[4], dados[5]);
    nova.giroscopio_x = juntar_bytes(dados[8], dados[9]);
    nova.giroscopio_y = juntar_bytes(dados[10], dados[11]);
    nova.giroscopio_z = juntar_bytes(dados[12], dados[13]);
    *leitura = nova;
    return ESP_OK;
}
