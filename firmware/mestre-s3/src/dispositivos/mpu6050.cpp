// Implementa configuração, verificação e leitura do MPU6050.
#include "dispositivos/mpu6050.h"

#include "gerenciadores/gerenciador_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao_placa.h"

namespace {
constexpr char ETIQUETA[] = "mpu6050";
constexpr uint8_t REGISTRO_DIVISOR_AMOSTRAGEM = 0x19;
constexpr uint8_t REGISTRO_CONFIGURACAO = 0x1A;
constexpr uint8_t REGISTRO_CONFIGURACAO_GIROSCOPIO = 0x1B;
constexpr uint8_t REGISTRO_CONFIGURACAO_ACELEROMETRO = 0x1C;
constexpr uint8_t REGISTRO_GERENCIAMENTO_ENERGIA = 0x6B;
constexpr uint8_t REGISTRO_DADOS_ACELERACAO = 0x3B;
constexpr uint8_t REGISTRO_IDENTIFICACAO = 0x75;
constexpr uint8_t IDENTIFICACAO_PADRAO_MPU6050 = 0x68;
constexpr uint16_t FREQUENCIA_BASE_COM_DLPF_HZ = 1000;
constexpr uint8_t CONFIGURACAO_DLPF_44_HZ = 0x03;
constexpr float GRAVIDADE_PADRAO_MS2 = 9.80665f;
constexpr float SENSIBILIDADE_ACELEROMETRO_LSB_G = 16384.0f;
constexpr float SENSIBILIDADE_GIROSCOPIO_LSB_GRAUS_S = 131.0f;
constexpr float SENSIBILIDADE_TEMPERATURA = 340.0f;
constexpr float TEMPERATURA_REFERENCIA_C = 36.53f;
EstadoMpu6050 estado{};
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;

int16_t juntar_bytes(uint8_t alto, uint8_t baixo) {
    return static_cast<int16_t>((static_cast<uint16_t>(alto) << 8) | baixo);
}

esp_err_t escrever_registro(uint8_t registro, uint8_t valor) {
    const uint8_t comando[] = {registro, valor};
    return gerenciador_i2c_escrever(
        configuracao::ENDERECO_MPU6050, comando, sizeof(comando),
        pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
}

esp_err_t ler_registro(uint8_t registro, uint8_t* valor) {
    return gerenciador_i2c_escrever_ler(
        configuracao::ENDERECO_MPU6050, &registro, sizeof(registro), valor,
        sizeof(*valor), pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
}

void registrar_resultado(esp_err_t erro, bool leitura) {
    portENTER_CRITICAL(&trava_estado);
    estado.ultimo_erro = erro;
    if (leitura) {
        if (erro == ESP_OK) {
            estado.leituras_ok++;
        } else {
            estado.erros_leitura++;
        }
    }
    portEXIT_CRITICAL(&trava_estado);
}

esp_err_t verificar_registro(uint8_t registro, uint8_t esperado,
                             uint8_t mascara = 0xFF) {
    uint8_t lido = 0;
    const esp_err_t erro = ler_registro(registro, &lido);
    if (erro != ESP_OK) return erro;
    if ((lido & mascara) != (esperado & mascara)) {
        ESP_LOGE(ETIQUETA,
                 "Registro 0x%02X incorreto: lido=0x%02X esperado=0x%02X mascara=0x%02X",
                 registro, lido, esperado, mascara);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

bool calcular_divisor_amostragem(uint32_t frequencia_solicitada_hz,
                                 uint8_t* divisor,
                                 uint16_t* frequencia_efetiva_hz) {
    if (divisor == nullptr || frequencia_efetiva_hz == nullptr ||
        frequencia_solicitada_hz < 4 ||
        frequencia_solicitada_hz > FREQUENCIA_BASE_COM_DLPF_HZ) {
        return false;
    }
    uint32_t fator = (FREQUENCIA_BASE_COM_DLPF_HZ +
                      frequencia_solicitada_hz / 2u) /
                     frequencia_solicitada_hz;
    if (fator < 1u) fator = 1u;
    if (fator > 256u) fator = 256u;
    *divisor = static_cast<uint8_t>(fator - 1u);
    *frequencia_efetiva_hz = static_cast<uint16_t>(
        FREQUENCIA_BASE_COM_DLPF_HZ / fator);
    return true;
}
}  // namespace

esp_err_t mpu6050_iniciar(uint32_t frequencia_amostragem_hz) {
    uint8_t divisor_amostragem = 0;
    uint16_t frequencia_efetiva_hz = 0;
    if (!calcular_divisor_amostragem(frequencia_amostragem_hz,
                                     &divisor_amostragem,
                                     &frequencia_efetiva_hz)) {
        return ESP_ERR_INVALID_ARG;
    }
    const EstadoMpu6050 estado_atual = mpu6050_obter_estado();
    if (estado_atual.inicializado) {
        return estado_atual.frequencia_amostragem_hz == frequencia_efetiva_hz
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }
    esp_err_t erro = gerenciador_i2c_iniciar();
    if (erro != ESP_OK) {
        registrar_resultado(erro, false);
        return erro;
    }

    // O reset coloca todos os registradores em um estado conhecido. O tempo de
    // espera cobre a partida documentada do dispositivo antes de selecionar o
    // PLL do giroscópio X como fonte de clock.
    erro = escrever_registro(REGISTRO_GERENCIAMENTO_ENERGIA, 0x80);
    if (erro == ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t identificacao = 0;
    if (erro == ESP_OK) erro = ler_registro(REGISTRO_IDENTIFICACAO, &identificacao);
    if (erro == ESP_OK) {
        portENTER_CRITICAL(&trava_estado);
        estado.identificacao = identificacao;
        portEXIT_CRITICAL(&trava_estado);
    }
    if (erro == ESP_OK && identificacao != IDENTIFICACAO_PADRAO_MPU6050) {
        // Alguns módulos vendidos como MPU6050 retornam uma identificação não
        // padrão, embora respondam corretamente no endereço I2C 0x68. A
        // confirmação funcional abaixo, por escrita e releitura, é decisiva.
        ESP_LOGW(ETIQUETA,
                 "WHO_AM_I nao padrao: 0x%02X (esperado 0x%02X); continuando a verificacao funcional",
                 identificacao, IDENTIFICACAO_PADRAO_MPU6050);
    }

    // Com o DLPF habilitado, a taxa de saída é 1 kHz e SMPLRT_DIV aplica
    // 1 kHz / (1 + divisor). DLPF_CFG=3 fornece aproximadamente 44 Hz no
    // acelerômetro e 42 Hz no giroscópio; as faixas escolhidas são ±2 g e
    // ±250 °/s, com sensibilidades de 16384 LSB/g e 131 LSB/(°/s).
    if (erro == ESP_OK) erro = escrever_registro(REGISTRO_GERENCIAMENTO_ENERGIA, 0x01);
    if (erro == ESP_OK) vTaskDelay(pdMS_TO_TICKS(30));
    if (erro == ESP_OK) erro = escrever_registro(
        REGISTRO_DIVISOR_AMOSTRAGEM, divisor_amostragem);
    if (erro == ESP_OK) erro = escrever_registro(
        REGISTRO_CONFIGURACAO, CONFIGURACAO_DLPF_44_HZ);
    if (erro == ESP_OK) erro = escrever_registro(REGISTRO_CONFIGURACAO_GIROSCOPIO, 0x00);
    if (erro == ESP_OK) erro = escrever_registro(REGISTRO_CONFIGURACAO_ACELEROMETRO, 0x00);
    // A confirmação por leitura detecta endereço errado, clone incompatível ou
    // barramento que aceita a escrita mas não mantém a configuração.
    // Confirma a fonte de clock e também que SLEEP, CYCLE e TEMP_DIS ficaram
    // desligados.
    if (erro == ESP_OK) erro = verificar_registro(REGISTRO_GERENCIAMENTO_ENERGIA, 0x01, 0x6F);
    if (erro == ESP_OK) erro = verificar_registro(
        REGISTRO_DIVISOR_AMOSTRAGEM, divisor_amostragem);
    if (erro == ESP_OK) erro = verificar_registro(
        REGISTRO_CONFIGURACAO, CONFIGURACAO_DLPF_44_HZ, 0x07);
    if (erro == ESP_OK) erro = verificar_registro(REGISTRO_CONFIGURACAO_GIROSCOPIO, 0x00, 0x18);
    if (erro == ESP_OK) erro = verificar_registro(REGISTRO_CONFIGURACAO_ACELEROMETRO, 0x00, 0x18);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Sensor inercial nao respondeu durante a inicializacao: %s",
                 esp_err_to_name(erro));
        registrar_resultado(erro, false);
        return erro;
    }
    portENTER_CRITICAL(&trava_estado);
    estado.inicializado = true;
    estado.configuracao_verificada = true;
    estado.identificacao = identificacao;
    estado.frequencia_amostragem_hz = frequencia_efetiva_hz;
    estado.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava_estado);
    ESP_LOGI(ETIQUETA,
             "MPU6050 pronto em 0x%02X: WHO_AM_I=0x%02X, %u Hz, DLPF acel=44 Hz/giro=42 Hz, ±2 g, ±250 graus/s",
             configuracao::ENDERECO_MPU6050, identificacao,
             static_cast<unsigned>(frequencia_efetiva_hz));
    return ESP_OK;
}

esp_err_t mpu6050_ler(LeituraMpu6050* leitura) {
    if (leitura == nullptr) return ESP_ERR_INVALID_ARG;
    if (!mpu6050_obter_estado().inicializado) return ESP_ERR_INVALID_STATE;

    uint8_t dados[14]{};
    const uint8_t registro = REGISTRO_DADOS_ACELERACAO;
    esp_err_t erro = gerenciador_i2c_escrever_ler(
        configuracao::ENDERECO_MPU6050, &registro, sizeof(registro), dados, sizeof(dados),
        pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
    if (erro != ESP_OK) {
        registrar_resultado(erro, true);
        return erro;
    }

    LeituraMpu6050 nova{};
    nova.aceleracao_x = juntar_bytes(dados[0], dados[1]);
    nova.aceleracao_y = juntar_bytes(dados[2], dados[3]);
    nova.aceleracao_z = juntar_bytes(dados[4], dados[5]);
    const int16_t temperatura_bruta = juntar_bytes(dados[6], dados[7]);
    nova.giroscopio_x = juntar_bytes(dados[8], dados[9]);
    nova.giroscopio_y = juntar_bytes(dados[10], dados[11]);
    nova.giroscopio_z = juntar_bytes(dados[12], dados[13]);
    nova.aceleracao_x_g = nova.aceleracao_x / SENSIBILIDADE_ACELEROMETRO_LSB_G;
    nova.aceleracao_y_g = nova.aceleracao_y / SENSIBILIDADE_ACELEROMETRO_LSB_G;
    nova.aceleracao_z_g = nova.aceleracao_z / SENSIBILIDADE_ACELEROMETRO_LSB_G;
    nova.aceleracao_x_ms2 = nova.aceleracao_x_g * GRAVIDADE_PADRAO_MS2;
    nova.aceleracao_y_ms2 = nova.aceleracao_y_g * GRAVIDADE_PADRAO_MS2;
    nova.aceleracao_z_ms2 = nova.aceleracao_z_g * GRAVIDADE_PADRAO_MS2;
    nova.giroscopio_x_graus_s =
        nova.giroscopio_x / SENSIBILIDADE_GIROSCOPIO_LSB_GRAUS_S;
    nova.giroscopio_y_graus_s =
        nova.giroscopio_y / SENSIBILIDADE_GIROSCOPIO_LSB_GRAUS_S;
    nova.giroscopio_z_graus_s =
        nova.giroscopio_z / SENSIBILIDADE_GIROSCOPIO_LSB_GRAUS_S;
    nova.temperatura_c = temperatura_bruta / SENSIBILIDADE_TEMPERATURA +
                         TEMPERATURA_REFERENCIA_C;
    const bool quadro_zerado = nova.aceleracao_x == 0 &&
                               nova.aceleracao_y == 0 &&
                               nova.aceleracao_z == 0 &&
                               nova.giroscopio_x == 0 &&
                               nova.giroscopio_y == 0 &&
                               nova.giroscopio_z == 0 &&
                               temperatura_bruta == 0;
    if (quadro_zerado || nova.temperatura_c < -50.0f ||
        nova.temperatura_c > 100.0f) {
        registrar_resultado(ESP_ERR_INVALID_RESPONSE, true);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *leitura = nova;
    registrar_resultado(ESP_OK, true);
    return ESP_OK;
}

EstadoMpu6050 mpu6050_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoMpu6050 copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}
