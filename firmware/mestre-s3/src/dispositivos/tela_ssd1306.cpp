#include "dispositivos/tela_ssd1306.h"

#include <cctype>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_i2c.h"
#include "nucleo/configuracao_placa.h"

namespace {
constexpr char ETIQUETA[] = "tela_ssd1306";
constexpr std::size_t COLUNAS = 128;
constexpr std::size_t PAGINAS = 8;
constexpr std::size_t TAMANHO_QUADRO = COLUNAS * PAGINAS;
constexpr std::size_t LARGURA_CARACTERE = 6;
constexpr std::size_t MAXIMO_CARACTERES_LINHA = COLUNAS / LARGURA_CARACTERE;
static_assert(configuracao::LARGURA_TELA_SSD1306 == COLUNAS &&
                  configuracao::ALTURA_TELA_SSD1306 == PAGINAS * 8,
              "O driver atual da SSD1306 requer uma tela 128x64");

constexpr uint8_t FONTE_NUMEROS[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
};

constexpr uint8_t FONTE_LETRAS[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

SemaphoreHandle_t mutex_tela = nullptr;
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;
EstadoTelaSsd1306 estado{};
uint8_t quadro_atual[TAMANHO_QUADRO]{};
uint8_t novo_quadro[TAMANHO_QUADRO]{};
bool quadro_atual_valido = false;

void registrar_resultado(esp_err_t erro) {
    portENTER_CRITICAL(&trava_estado);
    estado.ultimo_erro = erro;
    if (erro != ESP_OK) estado.erros++;
    portEXIT_CRITICAL(&trava_estado);
}

const uint8_t* obter_glifo(char caractere, uint8_t glifo_temporario[5]) {
    unsigned char codigo = static_cast<unsigned char>(caractere);
    if (codigo >= 'a' && codigo <= 'z') {
        codigo = static_cast<unsigned char>(std::toupper(codigo));
    }
    if (codigo >= '0' && codigo <= '9') return FONTE_NUMEROS[codigo - '0'];
    if (codigo >= 'A' && codigo <= 'Z') return FONTE_LETRAS[codigo - 'A'];

    std::memset(glifo_temporario, 0, 5);
    switch (codigo) {
        case '.': glifo_temporario[2] = 0x60; break;
        case ':':
            glifo_temporario[1] = 0x36;
            glifo_temporario[2] = 0x36;
            break;
        case '-':
            glifo_temporario[1] = 0x08;
            glifo_temporario[2] = 0x08;
            glifo_temporario[3] = 0x08;
            break;
        case '/':
            glifo_temporario[0] = 0x20;
            glifo_temporario[1] = 0x10;
            glifo_temporario[2] = 0x08;
            glifo_temporario[3] = 0x04;
            glifo_temporario[4] = 0x02;
            break;
        case '%':
            glifo_temporario[0] = 0x63;
            glifo_temporario[1] = 0x13;
            glifo_temporario[2] = 0x08;
            glifo_temporario[3] = 0x64;
            glifo_temporario[4] = 0x63;
            break;
        case '_':
            for (std::size_t coluna = 0; coluna < 5; ++coluna) {
                glifo_temporario[coluna] = 0x40;
            }
            break;
        case '|': glifo_temporario[2] = 0x7F; break;
        case '?':
            glifo_temporario[0] = 0x02;
            glifo_temporario[1] = 0x01;
            glifo_temporario[2] = 0x51;
            glifo_temporario[3] = 0x09;
            glifo_temporario[4] = 0x06;
            break;
        case ' ':
            break;
        default:
            glifo_temporario[0] = 0x02;
            glifo_temporario[1] = 0x01;
            glifo_temporario[2] = 0x51;
            glifo_temporario[3] = 0x09;
            glifo_temporario[4] = 0x06;
            break;
    }
    return glifo_temporario;
}

void desenhar_linha(std::size_t pagina, const char* texto) {
    if (pagina >= PAGINAS || texto == nullptr) return;
    for (std::size_t indice = 0;
         indice < MAXIMO_CARACTERES_LINHA && texto[indice] != '\0'; ++indice) {
        uint8_t temporario[5]{};
        const uint8_t* glifo = obter_glifo(texto[indice], temporario);
        const std::size_t x = indice * LARGURA_CARACTERE;
        for (std::size_t coluna = 0; coluna < 5; ++coluna) {
            novo_quadro[pagina * COLUNAS + x + coluna] = glifo[coluna];
        }
    }
}

void desenhar_barra_progresso(uint16_t percentual_decimos) {
    if (percentual_decimos > 1000) percentual_decimos = 1000;
    constexpr std::size_t PAGINA = PAGINAS - 1;
    constexpr std::size_t INTERIOR = COLUNAS - 2;
    const std::size_t preenchidas =
        (INTERIOR * static_cast<std::size_t>(percentual_decimos)) / 1000u;
    novo_quadro[PAGINA * COLUNAS] = 0xFF;
    novo_quadro[PAGINA * COLUNAS + COLUNAS - 1] = 0xFF;
    for (std::size_t coluna = 1; coluna < COLUNAS - 1; ++coluna) {
        novo_quadro[PAGINA * COLUNAS + coluna] =
            static_cast<uint8_t>(0x81u | (coluna <= preenchidas ? 0x7Eu : 0x00u));
    }
}

esp_err_t enviar_comandos(uint8_t endereco, const uint8_t* comandos,
                          std::size_t quantidade) {
    if (comandos == nullptr || quantidade == 0 || quantidade > 31) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t pacote[32]{};
    pacote[0] = 0x00;
    std::memcpy(&pacote[1], comandos, quantidade);
    return gerenciador_i2c_escrever(
        endereco, pacote, quantidade + 1,
        pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
}

esp_err_t enviar_quadro(uint8_t endereco, const uint8_t* quadro) {
    const uint8_t definir_area[] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
    esp_err_t erro = enviar_comandos(endereco, definir_area, sizeof(definir_area));
    if (erro != ESP_OK) return erro;

    uint8_t pacote[COLUNAS + 1]{};
    pacote[0] = 0x40;
    for (std::size_t pagina = 0; pagina < PAGINAS; ++pagina) {
        std::memcpy(&pacote[1], &quadro[pagina * COLUNAS], COLUNAS);
        erro = gerenciador_i2c_escrever(
            endereco, pacote, sizeof(pacote),
            pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS));
        if (erro != ESP_OK) return erro;
    }
    return ESP_OK;
}

uint8_t localizar_tela() {
    const uint8_t enderecos[] = {
        configuracao::ENDERECO_TELA_SSD1306_PRINCIPAL,
        configuracao::ENDERECO_TELA_SSD1306_ALTERNATIVO,
    };
    for (uint8_t endereco : enderecos) {
        if (gerenciador_i2c_sondar(
                endereco,
                pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_I2C_MS)) == ESP_OK) {
            return endereco;
        }
    }
    return 0;
}
}  // namespace

esp_err_t tela_ssd1306_iniciar() {
    if (tela_ssd1306_obter_estado().iniciada) return ESP_OK;
    if (mutex_tela == nullptr) mutex_tela = xSemaphoreCreateMutex();
    if (mutex_tela == nullptr) return ESP_ERR_NO_MEM;
    xSemaphoreTake(mutex_tela, portMAX_DELAY);

    esp_err_t erro = gerenciador_i2c_iniciar();
    if (erro != ESP_OK) {
        registrar_resultado(erro);
        xSemaphoreGive(mutex_tela);
        ESP_LOGE(ETIQUETA, "Não foi possível preparar o barramento I2C: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    // Dá tempo para o circuito de power-on reset dos módulos sem pino RST.
    vTaskDelay(pdMS_TO_TICKS(50));
    const uint8_t endereco = localizar_tela();
    if (endereco == 0) erro = ESP_ERR_NOT_FOUND;
    if (erro != ESP_OK) {
        registrar_resultado(erro);
        xSemaphoreGive(mutex_tela);
        ESP_LOGW(ETIQUETA,
                 "Tela não encontrada nos endereços 0x%02X e 0x%02X",
                 static_cast<unsigned>(configuracao::ENDERECO_TELA_SSD1306_PRINCIPAL),
                 static_cast<unsigned>(configuracao::ENDERECO_TELA_SSD1306_ALTERNATIVO));
        return erro;
    }

    const uint8_t inicializacao[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0x2E, 0xAF,
    };
    erro = enviar_comandos(endereco, inicializacao, sizeof(inicializacao));
    if (erro == ESP_OK) {
        std::memset(quadro_atual, 0, sizeof(quadro_atual));
        erro = enviar_quadro(endereco, quadro_atual);
    }

    portENTER_CRITICAL(&trava_estado);
    estado.presente = erro == ESP_OK;
    estado.iniciada = erro == ESP_OK;
    estado.endereco_i2c = erro == ESP_OK ? endereco : 0;
    estado.ultimo_erro = erro;
    if (erro == ESP_OK) {
        estado.quadros_enviados++;
    } else {
        estado.erros++;
    }
    portEXIT_CRITICAL(&trava_estado);
    quadro_atual_valido = erro == ESP_OK;
    xSemaphoreGive(mutex_tela);

    if (erro == ESP_OK) {
        ESP_LOGI(ETIQUETA, "SSD1306 128x64 pronta no endereço 0x%02X",
                 static_cast<unsigned>(endereco));
    } else {
        ESP_LOGE(ETIQUETA, "Falha ao inicializar a SSD1306: %s",
                 esp_err_to_name(erro));
    }
    return erro;
}

static esp_err_t exibir_conteudo(const char* const* linhas,
                                 std::size_t quantidade_linhas,
                                 int percentual_decimos) {
    const std::size_t maximo_linhas = percentual_decimos >= 0 ? PAGINAS - 1 : PAGINAS;
    if (quantidade_linhas > maximo_linhas ||
        (quantidade_linhas > 0 && linhas == nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }
    const EstadoTelaSsd1306 copia_estado = tela_ssd1306_obter_estado();
    if (!copia_estado.iniciada || !copia_estado.presente || mutex_tela == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(mutex_tela, portMAX_DELAY);
    std::memset(novo_quadro, 0, sizeof(novo_quadro));
    for (std::size_t pagina = 0; pagina < quantidade_linhas; ++pagina) {
        desenhar_linha(pagina, linhas[pagina]);
    }
    if (percentual_decimos >= 0) {
        desenhar_barra_progresso(static_cast<uint16_t>(percentual_decimos));
    }
    if (quadro_atual_valido &&
        std::memcmp(quadro_atual, novo_quadro, sizeof(quadro_atual)) == 0) {
        portENTER_CRITICAL(&trava_estado);
        estado.quadros_inalterados++;
        portEXIT_CRITICAL(&trava_estado);
        xSemaphoreGive(mutex_tela);
        return ESP_OK;
    }

    const esp_err_t erro = enviar_quadro(copia_estado.endereco_i2c, novo_quadro);
    if (erro == ESP_OK) {
        std::memcpy(quadro_atual, novo_quadro, sizeof(quadro_atual));
        quadro_atual_valido = true;
        portENTER_CRITICAL(&trava_estado);
        estado.quadros_enviados++;
        estado.ultimo_erro = ESP_OK;
        portEXIT_CRITICAL(&trava_estado);
    } else {
        quadro_atual_valido = false;
        registrar_resultado(erro);
    }
    xSemaphoreGive(mutex_tela);
    return erro;
}

esp_err_t tela_ssd1306_exibir_linhas(const char* const* linhas,
                                     std::size_t quantidade_linhas) {
    return exibir_conteudo(linhas, quantidade_linhas, -1);
}

esp_err_t tela_ssd1306_exibir_linhas_com_progresso(
    const char* const* linhas, std::size_t quantidade_linhas,
    uint16_t percentual_decimos) {
    if (percentual_decimos > 1000) return ESP_ERR_INVALID_ARG;
    return exibir_conteudo(linhas, quantidade_linhas, percentual_decimos);
}

esp_err_t tela_ssd1306_limpar() {
    return tela_ssd1306_exibir_linhas(nullptr, 0);
}

EstadoTelaSsd1306 tela_ssd1306_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoTelaSsd1306 copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}
