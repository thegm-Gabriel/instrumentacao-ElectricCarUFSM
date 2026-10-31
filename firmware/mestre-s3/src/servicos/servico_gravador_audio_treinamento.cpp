// Implementa a ferramenta temporária de aquisição do INMP441 para o cartão SD.
#include "servicos/servico_gravador_audio_treinamento.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nucleo/configuracao_placa.h"
#include "nucleo/configuracao_registro_telemetria.h"

namespace {
constexpr char ETIQUETA[] = "gravador_audio";
constexpr char DIRETORIO_AUDIO[] = "/sdcard/audio_ia";
constexpr uint32_t FREQUENCIA_AMOSTRAGEM_HZ = 16000;
constexpr uint16_t QUANTIDADE_CANAIS_WAV = 1;
constexpr uint16_t BITS_POR_AMOSTRA_WAV = 16;
constexpr uint32_t DURACAO_MINIMA_SEGUNDOS = 1;
constexpr uint32_t DURACAO_MAXIMA_SEGUNDOS = 60;
constexpr std::size_t TAMANHO_MAXIMO_NOME = 48;
constexpr std::size_t QUADROS_POR_BLOCO = 256;
constexpr std::size_t AMOSTRAS_I2S_POR_QUADRO = 2;
// O INMP441 entrega 24 bits alinhados à esquerda em uma palavra de 32 bits.
// O deslocamento 14 preserva ganho útil para voz sem amplificação digital
// agressiva; pico/saturação informados ao final ajudam a ajustar este valor.
constexpr uint8_t DESLOCAMENTO_PARA_PCM16 = 14;

void escrever_u16_le(uint8_t* destino, uint16_t valor) {
    destino[0] = static_cast<uint8_t>(valor);
    destino[1] = static_cast<uint8_t>(valor >> 8U);
}

void escrever_u32_le(uint8_t* destino, uint32_t valor) {
    destino[0] = static_cast<uint8_t>(valor);
    destino[1] = static_cast<uint8_t>(valor >> 8U);
    destino[2] = static_cast<uint8_t>(valor >> 16U);
    destino[3] = static_cast<uint8_t>(valor >> 24U);
}

void montar_cabecalho_wav(uint8_t* cabecalho, uint32_t bytes_pcm) {
    std::memset(cabecalho, 0, 44);
    std::memcpy(cabecalho, "RIFF", 4);
    escrever_u32_le(cabecalho + 4, 36U + bytes_pcm);
    std::memcpy(cabecalho + 8, "WAVEfmt ", 8);
    escrever_u32_le(cabecalho + 16, 16);
    escrever_u16_le(cabecalho + 20, 1);
    escrever_u16_le(cabecalho + 22, QUANTIDADE_CANAIS_WAV);
    escrever_u32_le(cabecalho + 24, FREQUENCIA_AMOSTRAGEM_HZ);
    const uint32_t bytes_por_segundo =
        FREQUENCIA_AMOSTRAGEM_HZ * QUANTIDADE_CANAIS_WAV *
        (BITS_POR_AMOSTRA_WAV / 8U);
    escrever_u32_le(cabecalho + 28, bytes_por_segundo);
    escrever_u16_le(cabecalho + 32,
                    QUANTIDADE_CANAIS_WAV * (BITS_POR_AMOSTRA_WAV / 8U));
    escrever_u16_le(cabecalho + 34, BITS_POR_AMOSTRA_WAV);
    std::memcpy(cabecalho + 36, "data", 4);
    escrever_u32_le(cabecalho + 40, bytes_pcm);
}

bool criar_diretorio(const char* caminho) {
    return mkdir(caminho, 0775) == 0 || errno == EEXIST;
}

bool normalizar_nome(const char* origem, char* destino, std::size_t capacidade) {
    if (origem == nullptr || destino == nullptr || capacidade < 2) return false;
    std::size_t usados = 0;
    bool separador_anterior = false;
    for (const unsigned char* atual =
             reinterpret_cast<const unsigned char*>(origem);
         *atual != '\0' && usados + 1 < capacidade; ++atual) {
        const bool letra = (*atual >= 'a' && *atual <= 'z') ||
                           (*atual >= 'A' && *atual <= 'Z');
        const bool numero = *atual >= '0' && *atual <= '9';
        if (letra || numero || *atual == '-' || *atual == '_') {
            destino[usados++] = static_cast<char>(*atual);
            separador_anterior = *atual == '-' || *atual == '_';
        } else if (!separador_anterior && usados > 0) {
            destino[usados++] = '_';
            separador_anterior = true;
        }
    }
    while (usados > 0 && (destino[usados - 1] == '_' ||
                           destino[usados - 1] == '-')) {
        usados--;
    }
    destino[usados] = '\0';
    return usados > 0;
}

esp_err_t finalizar_wav(std::FILE* arquivo, uint32_t bytes_pcm) {
    uint8_t cabecalho[44]{};
    montar_cabecalho_wav(cabecalho, bytes_pcm);
    if (std::fseek(arquivo, 0, SEEK_SET) != 0 ||
        std::fwrite(cabecalho, 1, sizeof(cabecalho), arquivo) !=
            sizeof(cabecalho) ||
        std::fflush(arquivo) != 0 || fsync(fileno(arquivo)) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t iniciar_i2s(i2s_chan_handle_t* canal_rx) {
    if (canal_rx == nullptr) return ESP_ERR_INVALID_ARG;
    *canal_rx = nullptr;

    i2s_chan_config_t canal =
        I2S_CHANNEL_DEFAULT_CONFIG(configuracao::PORTA_I2S_MICROFONE,
                                   I2S_ROLE_MASTER);
    canal.dma_desc_num = 6;
    canal.dma_frame_num = 256;
    esp_err_t erro = i2s_new_channel(&canal, nullptr, canal_rx);
    if (erro != ESP_OK) return erro;

    i2s_std_config_t padrao{};
    padrao.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FREQUENCIA_AMOSTRAGEM_HZ);
    padrao.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    padrao.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    padrao.gpio_cfg.bclk = configuracao::PINO_MICROFONE_I2S_BCLK;
    padrao.gpio_cfg.ws = configuracao::PINO_MICROFONE_I2S_WS;
    padrao.gpio_cfg.dout = I2S_GPIO_UNUSED;
    padrao.gpio_cfg.din = configuracao::PINO_MICROFONE_I2S_DADOS;
    padrao.gpio_cfg.invert_flags = {};
    erro = i2s_channel_init_std_mode(*canal_rx, &padrao);
    if (erro == ESP_OK) erro = i2s_channel_enable(*canal_rx);
    if (erro != ESP_OK) {
        i2s_del_channel(*canal_rx);
        *canal_rx = nullptr;
    }
    return erro;
}

void encerrar_i2s(i2s_chan_handle_t* canal_rx) {
    if (canal_rx == nullptr || *canal_rx == nullptr) return;
    const esp_err_t erro_desabilitar = i2s_channel_disable(*canal_rx);
    if (erro_desabilitar != ESP_OK &&
        erro_desabilitar != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ETIQUETA, "Falha ao parar I2S: %s",
                 esp_err_to_name(erro_desabilitar));
    }
    i2s_del_channel(*canal_rx);
    *canal_rx = nullptr;
}
}  // namespace

esp_err_t servico_gravador_audio_treinamento_gravar(
    const char* nome, uint32_t duracao_segundos,
    ResultadoGravacaoAudioTreinamento* resultado) {
    if (resultado == nullptr || duracao_segundos < DURACAO_MINIMA_SEGUNDOS ||
        duracao_segundos > DURACAO_MAXIMA_SEGUNDOS) {
        return ESP_ERR_INVALID_ARG;
    }
    *resultado = {};

    char nome_normalizado[TAMANHO_MAXIMO_NOME + 1]{};
    if (!normalizar_nome(nome, nome_normalizado, sizeof(nome_normalizado))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!criar_diretorio(DIRETORIO_AUDIO)) {
        ESP_LOGE(ETIQUETA, "Não foi possível criar %s: errno=%d",
                 DIRETORIO_AUDIO, errno);
        return ESP_FAIL;
    }
    const int escritos = std::snprintf(
        resultado->caminho, sizeof(resultado->caminho), "%s/%s.wav",
        DIRETORIO_AUDIO, nome_normalizado);
    if (escritos <= 0 ||
        static_cast<std::size_t>(escritos) >= sizeof(resultado->caminho)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (access(resultado->caminho, F_OK) == 0) {
        resultado->arquivo_existente = true;
        return ESP_ERR_INVALID_STATE;
    }

    std::FILE* arquivo = std::fopen(resultado->caminho, "wx");
    if (arquivo == nullptr) {
        resultado->arquivo_existente = errno == EEXIST;
        return resultado->arquivo_existente ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }

    uint8_t cabecalho_vazio[44]{};
    if (std::fwrite(cabecalho_vazio, 1, sizeof(cabecalho_vazio), arquivo) !=
        sizeof(cabecalho_vazio)) {
        std::fclose(arquivo);
        unlink(resultado->caminho);
        return ESP_FAIL;
    }

    i2s_chan_handle_t canal_rx = nullptr;
    esp_err_t erro = iniciar_i2s(&canal_rx);
    if (erro != ESP_OK) {
        std::fclose(arquivo);
        unlink(resultado->caminho);
        ESP_LOGE(ETIQUETA, "Não foi possível iniciar o INMP441: %s",
                 esp_err_to_name(erro));
        return erro;
    }

    const std::size_t quantidade_bruta =
        QUADROS_POR_BLOCO * AMOSTRAS_I2S_POR_QUADRO;
    auto* bruto = static_cast<int32_t*>(heap_caps_malloc(
        quantidade_bruta * sizeof(int32_t), MALLOC_CAP_8BIT));
    auto* pcm = static_cast<int16_t*>(heap_caps_malloc(
        QUADROS_POR_BLOCO * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bruto == nullptr || pcm == nullptr) {
        heap_caps_free(bruto);
        heap_caps_free(pcm);
        encerrar_i2s(&canal_rx);
        std::fclose(arquivo);
        unlink(resultado->caminho);
        return ESP_ERR_NO_MEM;
    }

    const uint32_t total_amostras =
        duracao_segundos * FREQUENCIA_AMOSTRAGEM_HZ;
    const int64_t inicio_us = esp_timer_get_time();
    uint32_t proximo_relatorio = FREQUENCIA_AMOSTRAGEM_HZ;
    ESP_LOGI(ETIQUETA, "Gravando %s por %lu s", resultado->caminho,
             static_cast<unsigned long>(duracao_segundos));

    while (resultado->amostras_gravadas < total_amostras) {
        size_t bytes_lidos = 0;
        erro = i2s_channel_read(canal_rx, bruto,
                                quantidade_bruta * sizeof(int32_t),
                                &bytes_lidos, 1000);
        if (erro != ESP_OK) break;
        const std::size_t quadros_lidos =
            bytes_lidos / (AMOSTRAS_I2S_POR_QUADRO * sizeof(int32_t));
        if (quadros_lidos == 0) {
            erro = ESP_ERR_TIMEOUT;
            break;
        }
        const std::size_t restantes =
            total_amostras - resultado->amostras_gravadas;
        const std::size_t quantidade_pcm = std::min(quadros_lidos, restantes);
        for (std::size_t i = 0; i < quantidade_pcm; ++i) {
            const std::size_t canal =
                configuracao::MICROFONE_I2S_CANAL_ESQUERDO ? 0 : 1;
            const int32_t amostra =
                bruto[i * AMOSTRAS_I2S_POR_QUADRO + canal] >>
                DESLOCAMENTO_PARA_PCM16;
            if (amostra > INT16_MAX || amostra < INT16_MIN) {
                resultado->amostras_saturadas++;
            }
            const int32_t limitada =
                std::max<int32_t>(INT16_MIN, std::min<int32_t>(INT16_MAX, amostra));
            pcm[i] = static_cast<int16_t>(limitada);
            const int32_t absoluto = limitada == INT16_MIN ? INT16_MAX
                                                           : std::abs(limitada);
            resultado->pico_absoluto = static_cast<int16_t>(
                std::max<int32_t>(resultado->pico_absoluto, absoluto));
        }
        if (std::fwrite(pcm, sizeof(int16_t), quantidade_pcm, arquivo) !=
            quantidade_pcm) {
            erro = ESP_FAIL;
            break;
        }
        resultado->amostras_gravadas += quantidade_pcm;
        if (resultado->amostras_gravadas >= proximo_relatorio) {
            const uint32_t segundos =
                resultado->amostras_gravadas / FREQUENCIA_AMOSTRAGEM_HZ;
            ESP_LOGI(ETIQUETA, "Gravação: %lu/%lu s",
                     static_cast<unsigned long>(segundos),
                     static_cast<unsigned long>(duracao_segundos));
            proximo_relatorio += FREQUENCIA_AMOSTRAGEM_HZ;
        }
    }

    encerrar_i2s(&canal_rx);
    heap_caps_free(bruto);
    heap_caps_free(pcm);
    const uint32_t bytes_pcm =
        resultado->amostras_gravadas * sizeof(int16_t);
    const esp_err_t erro_finalizacao = finalizar_wav(arquivo, bytes_pcm);
    std::fclose(arquivo);
    resultado->duracao_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - inicio_us) / 1000LL);
    resultado->arquivo_parcial =
        resultado->amostras_gravadas != total_amostras;

    if (erro == ESP_OK && erro_finalizacao != ESP_OK) erro = erro_finalizacao;
    if (resultado->amostras_gravadas == 0) {
        unlink(resultado->caminho);
        resultado->caminho[0] = '\0';
    }
    if (erro == ESP_OK) {
        ESP_LOGI(ETIQUETA,
                 "WAV concluído: %s | %lu amostras | pico=%d | saturadas=%lu",
                 resultado->caminho,
                 static_cast<unsigned long>(resultado->amostras_gravadas),
                 resultado->pico_absoluto,
                 static_cast<unsigned long>(resultado->amostras_saturadas));
    } else {
        ESP_LOGE(ETIQUETA, "Gravação interrompida: %s", esp_err_to_name(erro));
    }
    return erro;
}
