// Converte os registros do INA228 conforme a folha de dados da TI (SLYS021A).
#include "dispositivos/ina228.h"

#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gerenciadores/gerenciador_i2c.h"
#include "nucleo/configuracao_placa.h"

namespace {
constexpr char ETIQUETA[] = "ina228";
constexpr uint8_t REG_CONFIG = 0x00;
constexpr uint8_t REG_ADC_CONFIG = 0x01;
constexpr uint8_t REG_SHUNT_CAL = 0x02;
constexpr uint8_t REG_VSHUNT = 0x04;
constexpr uint8_t REG_VBUS = 0x05;
constexpr uint8_t REG_DIETEMP = 0x06;
constexpr uint8_t REG_CURRENT = 0x07;
constexpr uint8_t REG_POWER = 0x08;
constexpr uint8_t REG_ENERGY = 0x09;
constexpr uint8_t REG_CHARGE = 0x0A;
constexpr uint8_t REG_DIAG_ALRT = 0x0B;
constexpr uint8_t REG_MANUFACTURER_ID = 0x3E;
constexpr uint8_t REG_DEVICE_ID = 0x3F;
constexpr uint16_t ID_TI = 0x5449;
constexpr uint16_t ID_INA228 = 0x2280;  // Ignora os 4 bits de revisão.
constexpr uint16_t CONFIG_FAIXA_SHUNT_163_MV = 0x0000;
// Modo contínuo VBUS + VSHUNT + temperatura, 540 us/canal, média de 16.
constexpr uint16_t ADC_CONFIG_CONTINUO = 0xF922;
constexpr uint16_t DIAG_ERRO_MATEMATICO = 1u << 9;
constexpr uint16_t DIAG_OVERFLOW_ENERGIA = 1u << 11;
constexpr uint16_t DIAG_OVERFLOW_CARGA = 1u << 10;
constexpr uint16_t DIAG_CONVERSAO_PRONTA = 1u << 1;
constexpr uint16_t DIAG_MEMORIA_OK = 1u << 0;
constexpr uint64_t MASCARA_40_BITS = (1ULL << 40) - 1ULL;

EstadoIna228 estado{};
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;
float current_lsb_a = 0.0f;

esp_err_t ler_registro(uint8_t registro, uint8_t* bytes, size_t quantidade) {
    return gerenciador_i2c_escrever_ler(
        configuracao::ENDERECO_INA228, &registro, 1, bytes, quantidade,
        pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
}

esp_err_t ler_u16(uint8_t registro, uint16_t* valor) {
    uint8_t bytes[2]{};
    const esp_err_t erro = ler_registro(registro, bytes, sizeof(bytes));
    if (erro == ESP_OK) {
        *valor = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
    }
    return erro;
}

esp_err_t ler_u24(uint8_t registro, uint32_t* valor) {
    uint8_t bytes[3]{};
    const esp_err_t erro = ler_registro(registro, bytes, sizeof(bytes));
    if (erro == ESP_OK) {
        *valor = (static_cast<uint32_t>(bytes[0]) << 16) |
                 (static_cast<uint32_t>(bytes[1]) << 8) | bytes[2];
    }
    return erro;
}

esp_err_t ler_u40(uint8_t registro, uint64_t* valor) {
    uint8_t bytes[5]{};
    const esp_err_t erro = ler_registro(registro, bytes, sizeof(bytes));
    if (erro == ESP_OK) {
        uint64_t resultado = 0;
        for (uint8_t byte : bytes) resultado = (resultado << 8) | byte;
        *valor = resultado;
    }
    return erro;
}

esp_err_t escrever_u16(uint8_t registro, uint16_t valor) {
    const uint8_t bytes[] = {
        registro, static_cast<uint8_t>(valor >> 8),
        static_cast<uint8_t>(valor & 0xFFu)};
    return gerenciador_i2c_escrever(
        configuracao::ENDERECO_INA228, bytes, sizeof(bytes),
        pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
}

int32_t sinalizar_20_bits(uint32_t bruto) {
    const int32_t valor = static_cast<int32_t>(bruto >> 4);
    return (valor & (1 << 19)) != 0 ? valor - (1 << 20) : valor;
}

int64_t sinalizar_40_bits(uint64_t bruto) {
    const int64_t valor = static_cast<int64_t>(bruto & MASCARA_40_BITS);
    return (bruto & (1ULL << 39)) != 0 ? valor - (1LL << 40) : valor;
}

void registrar_erro(esp_err_t erro) {
    portENTER_CRITICAL(&trava_estado);
    estado.erros_leitura++;
    estado.erros_consecutivos++;
    estado.ultimo_erro = erro;
    estado.ultima_leitura.valida = false;
    portEXIT_CRITICAL(&trava_estado);
}
}  // namespace

esp_err_t ina228_iniciar() {
    const float shunt = configuracao::RESISTENCIA_SHUNT_INA228_OHM;
    const float limite = configuracao::CORRENTE_MAXIMA_INA228_A;
    if (!std::isfinite(shunt) || !std::isfinite(limite) ||
        shunt <= 0.0f || limite <= 0.0f ||
        (configuracao::SINAL_CORRENTE_INA228 != 1.0f &&
         configuracao::SINAL_CORRENTE_INA228 != -1.0f) ||
        limite * shunt >= 0.16384f) return ESP_ERR_INVALID_ARG;

    const float lsb = limite / 524288.0f;
    const long calibracao = std::lround(13107200000.0 *
                                       static_cast<double>(lsb) * shunt);
    if (calibracao <= 0 || calibracao > 0x7FFF) return ESP_ERR_INVALID_ARG;

    esp_err_t erro = gerenciador_i2c_iniciar();
    uint16_t fabricante = 0;
    uint16_t identificacao = 0;
    if (erro == ESP_OK) erro = ler_u16(REG_MANUFACTURER_ID, &fabricante);
    if (erro == ESP_OK) erro = ler_u16(REG_DEVICE_ID, &identificacao);
    if (erro == ESP_OK &&
        (fabricante != ID_TI || (identificacao & 0xFFF0u) != ID_INA228)) {
        erro = ESP_ERR_INVALID_RESPONSE;
    }
    if (erro == ESP_OK) erro = escrever_u16(REG_CONFIG, CONFIG_FAIXA_SHUNT_163_MV);
    if (erro == ESP_OK) erro = escrever_u16(REG_SHUNT_CAL, static_cast<uint16_t>(calibracao));
    if (erro == ESP_OK) erro = escrever_u16(REG_ADC_CONFIG, ADC_CONFIG_CONTINUO);
    uint16_t conferido = 0;
    if (erro == ESP_OK) erro = ler_u16(REG_SHUNT_CAL, &conferido);
    if (erro == ESP_OK && conferido != calibracao) erro = ESP_ERR_INVALID_RESPONSE;

    portENTER_CRITICAL(&trava_estado);
    estado.inicializado = erro == ESP_OK;
    estado.identificacao = identificacao;
    estado.calibracao = erro == ESP_OK ? static_cast<uint16_t>(calibracao) : 0;
    estado.ultimo_erro = erro;
    estado.ultima_leitura.valida = false;
    portEXIT_CRITICAL(&trava_estado);
    if (erro != ESP_OK) return erro;
    current_lsb_a = lsb;
    ESP_LOGI(ETIQUETA,
             "INA228 0x%02X pronto: shunt=%.3f mOhm, Imax=%.1f A, SHUNT_CAL=%ld",
             configuracao::ENDERECO_INA228, shunt * 1000.0f, limite, calibracao);
    return ESP_OK;
}

esp_err_t ina228_ler(LeituraIna228* destino) {
    if (destino == nullptr) return ESP_ERR_INVALID_ARG;
    *destino = {};
    if (!ina228_obter_estado().inicializado) return ESP_ERR_INVALID_STATE;

    uint16_t calibracao = 0;
    uint16_t diagnostico = 0;
    uint16_t temperatura = 0;
    uint32_t shunt = 0, barramento = 0, corrente = 0, potencia = 0;
    uint64_t energia = 0, carga = 0;
    esp_err_t erro = ler_u16(REG_SHUNT_CAL, &calibracao);
    if (erro == ESP_OK && calibracao != ina228_obter_estado().calibracao)
        erro = ESP_ERR_INVALID_STATE;  // Reset ou perda da calibração.
    if (erro == ESP_OK) erro = ler_u16(REG_DIAG_ALRT, &diagnostico);
    if (erro == ESP_OK && (diagnostico & DIAG_MEMORIA_OK) == 0)
        erro = ESP_ERR_INVALID_RESPONSE;
    if (erro == ESP_OK && (diagnostico & DIAG_ERRO_MATEMATICO) != 0)
        erro = ESP_ERR_INVALID_RESPONSE;
    if (erro == ESP_OK && (diagnostico & DIAG_CONVERSAO_PRONTA) == 0)
        erro = ESP_ERR_INVALID_STATE;
    if (erro == ESP_OK) erro = ler_u24(REG_VSHUNT, &shunt);
    if (erro == ESP_OK) erro = ler_u24(REG_VBUS, &barramento);
    if (erro == ESP_OK) erro = ler_u16(REG_DIETEMP, &temperatura);
    if (erro == ESP_OK) erro = ler_u24(REG_CURRENT, &corrente);
    if (erro == ESP_OK) erro = ler_u24(REG_POWER, &potencia);
    if (erro == ESP_OK && (diagnostico & DIAG_OVERFLOW_ENERGIA) == 0)
        erro = ler_u40(REG_ENERGY, &energia);
    if (erro == ESP_OK && (diagnostico & DIAG_OVERFLOW_CARGA) == 0)
        erro = ler_u40(REG_CHARGE, &carga);
    if (erro != ESP_OK) {
        registrar_erro(erro);
        return erro;
    }

    LeituraIna228 leitura{};
    leitura.tensao_shunt_mv = sinalizar_20_bits(shunt) * 0.0003125f *
                              configuracao::SINAL_CORRENTE_INA228;
    leitura.tensao_barramento_v = (barramento >> 4) * 0.0001953125f;
    leitura.corrente_a = sinalizar_20_bits(corrente) * current_lsb_a *
                         configuracao::SINAL_CORRENTE_INA228;
    leitura.potencia_w = leitura.tensao_barramento_v * leitura.corrente_a;
    leitura.potencia_chip_w = potencia * 3.2f * current_lsb_a;
    leitura.temperatura_chip_c = static_cast<int16_t>(temperatura) * 0.0078125f;
    leitura.energia_valida = (diagnostico & DIAG_OVERFLOW_ENERGIA) == 0;
    leitura.carga_valida = (diagnostico & DIAG_OVERFLOW_CARGA) == 0;
    if (leitura.energia_valida) {
        leitura.energia_j = static_cast<double>(energia) * 51.2 * current_lsb_a;
    }
    if (leitura.carga_valida) {
        leitura.carga_c = static_cast<double>(sinalizar_40_bits(carga)) *
                          current_lsb_a * configuracao::SINAL_CORRENTE_INA228;
    }
    leitura.diagnostico = diagnostico;
    leitura.instante_ms = esp_timer_get_time() / 1000;
    leitura.valida = std::isfinite(leitura.tensao_barramento_v) &&
                     std::isfinite(leitura.corrente_a) &&
                     std::isfinite(leitura.potencia_w) &&
                     leitura.temperatura_chip_c >= -40.0f &&
                     leitura.temperatura_chip_c <= 125.0f &&
                     leitura.tensao_barramento_v <= 65.535f &&
                     std::fabs(leitura.corrente_a) <=
                         configuracao::CORRENTE_MAXIMA_INA228_A * 1.01f;
    if (!leitura.valida) {
        registrar_erro(ESP_ERR_INVALID_RESPONSE);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *destino = leitura;
    portENTER_CRITICAL(&trava_estado);
    estado.ultima_leitura = leitura;
    estado.leituras_ok++;
    estado.erros_consecutivos = 0;
    estado.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava_estado);
    return ESP_OK;
}

EstadoIna228 ina228_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoIna228 copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}
