#include "dispositivos/saidas/mcp4725.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gerenciadores/i2c.h"
#include "nucleo/configuracao/placa.h"

namespace {
constexpr char ETIQUETA[] = "mcp4725";
bool presente = false;
constexpr TickType_t PRAZO_I2C = pdMS_TO_TICKS(configuracao::PRAZO_I2C_MCP4725_MS);
static_assert(PRAZO_I2C > 0);
}

esp_err_t mcp4725_definir_milivolts(uint16_t milivolts) {
    if (milivolts > configuracao::MCP4725_ALIMENTACAO_MV)
        return ESP_ERR_INVALID_ARG;
    if (!presente) return ESP_ERR_INVALID_STATE;
    const uint16_t codigo = static_cast<uint16_t>(
        (static_cast<uint32_t>(milivolts) * 4095u +
         configuracao::MCP4725_ALIMENTACAO_MV / 2u) /
        configuracao::MCP4725_ALIMENTACAO_MV);
    // Fast write (C2/C1=00, PD=00): atualiza apenas o registrador DAC.
    const uint8_t quadro[2] = {
        static_cast<uint8_t>(codigo >> 8),
        static_cast<uint8_t>(codigo & 0xffu),
    };
    const esp_err_t erro = gerenciador_i2c_escrever(
        configuracao::ENDERECO_MCP4725, quadro, sizeof(quadro), PRAZO_I2C);
    if (erro != ESP_OK) presente = false;
    return erro;
}

esp_err_t mcp4725_iniciar() {
    const esp_err_t erro = gerenciador_i2c_sondar(
        configuracao::ENDERECO_MCP4725, PRAZO_I2C);
    if (erro != ESP_OK) {
        presente = false;
        return erro;
    }
    presente = true;
    const esp_err_t zerar = mcp4725_definir_milivolts(0);
    if (zerar != ESP_OK) return zerar;
    ESP_LOGI(ETIQUETA, "DAC em 0 V; endereço 0x%02x; EEPROM preservada",
             configuracao::ENDERECO_MCP4725);
    return ESP_OK;
}

bool mcp4725_disponivel() { return presente; }
