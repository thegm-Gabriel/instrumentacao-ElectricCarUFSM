// Valida o protocolo e a recuperação do parser UART usado em produção.
#include <cstring>

#include "protocolo_enlace_uart.h"
#include "protocolo_ota_satelites.h"
#include "protocolo_telemetria.h"
#include "unity.h"

namespace {
pacote_telemetria_t criar_pacote_valido(uint16_t sequencia = 10) {
    pacote_telemetria_t pacote{};
    pacote.inicio_1 = TELEMETRIA_INICIO_1;
    pacote.inicio_2 = TELEMETRIA_INICIO_2;
    pacote.versao = TELEMETRIA_VERSAO_PROTOCOLO;
    pacote.tipo = TELEMETRIA_TIPO_DADOS;
    pacote.tamanho_carga = TELEMETRIA_TAMANHO_CARGA_DADOS;
    pacote.sequencia = sequencia;
    pacote.tensao_adc_bruta = 2048;
    pacote.marcha = 2;
    pacote.velocidade_centesimos_kmh = 4250;
    pacote.acelerador_decimos_percentual = 300;
    pacote.freio_decimos_percentual = 0;
    pacote.carga_decimos_percentual = 750;
    pacote.latitude_micrograus = -30033900;
    pacote.longitude_micrograus = -52893100;
    pacote.rumo_decimos_grau = 1800;
    pacote.temperatura_mpu_centesimos_c = 2534;
    pacote.estado_mpu = TELEMETRIA_MPU_ATIVO | TELEMETRIA_MPU_CALIBRADO |
                        TELEMETRIA_MPU_EIXO_COMPATIVEL |
                        (TELEMETRIA_MPU_SENTIDO_FRENTE <<
                         TELEMETRIA_MPU_SENTIDO_DESLOCAMENTO) |
                        (1u << TELEMETRIA_MPU_EIXO_DESLOCAMENTO);
    pacote.crc16 = protocolo_telemetria_calcular_crc16(&pacote);
    return pacote;
}

struct ResultadoFluxo {
    unsigned pacotes_validos = 0;
    unsigned pacotes_invalidos = 0;
};

void alimentar(parser_enlace_uart_t& parser, ResultadoFluxo& resultado,
               const uint8_t* bytes, size_t tamanho) {
    for (size_t indice = 0; indice < tamanho; ++indice) {
        const resultado_parser_enlace_uart_t evento =
            protocolo_enlace_uart_processar_byte(&parser, bytes[indice]);
        if (evento == ENLACE_UART_QUADRO_INVALIDO) {
            resultado.pacotes_invalidos++;
            continue;
        }
        if (evento != ENLACE_UART_TELEMETRIA_COMPLETA) continue;

        pacote_telemetria_t recebido{};
        std::memcpy(&recebido, parser.quadro, sizeof(recebido));
        if (protocolo_telemetria_pacote_valido(&recebido) &&
            protocolo_telemetria_conteudo_valido(&recebido)) {
            resultado.pacotes_validos++;
        } else {
            resultado.pacotes_invalidos++;
            protocolo_enlace_uart_reaproveitar_ultimo_quadro(&parser);
        }
    }
}
}  // namespace

void setUp() {}
void tearDown() {}

void testar_pacote_valido_e_tamanho() {
    const pacote_telemetria_t pacote = criar_pacote_valido();
    TEST_ASSERT_EQUAL_UINT32(TAMANHO_PACOTE_TELEMETRIA, sizeof(pacote));
    TEST_ASSERT_TRUE(protocolo_telemetria_pacote_valido(&pacote));
    TEST_ASSERT_TRUE(protocolo_telemetria_conteudo_valido(&pacote));
}

void testar_crc_detecta_corrupcao() {
    pacote_telemetria_t pacote = criar_pacote_valido();
    pacote.aceleracao_x ^= 0x0040;
    TEST_ASSERT_FALSE(protocolo_telemetria_pacote_valido(&pacote));
}

void testar_limites_de_conteudo() {
    pacote_telemetria_t pacote = criar_pacote_valido();
    pacote.carga_decimos_percentual = 1001;
    pacote.crc16 = protocolo_telemetria_calcular_crc16(&pacote);
    TEST_ASSERT_TRUE(protocolo_telemetria_pacote_valido(&pacote));
    TEST_ASSERT_FALSE(protocolo_telemetria_conteudo_valido(&pacote));

    pacote = criar_pacote_valido();
    pacote.estado_mpu |= 0x80u;
    pacote.crc16 = protocolo_telemetria_calcular_crc16(&pacote);
    TEST_ASSERT_FALSE(protocolo_telemetria_conteudo_valido(&pacote));
}

void testar_confirmacao_e_corrupcao() {
    confirmacao_telemetria_t confirmacao{};
    confirmacao.inicio_1 = TELEMETRIA_INICIO_1;
    confirmacao.inicio_2 = TELEMETRIA_INICIO_CONFIRMACAO_2;
    confirmacao.versao = TELEMETRIA_VERSAO_PROTOCOLO;
    confirmacao.tipo = TELEMETRIA_TIPO_CONFIRMACAO;
    confirmacao.sequencia = 25;
    confirmacao.tempo_mestre_ms = 12345;
    confirmacao.tipo_satelite = SATELITE_TELEMETRIA_EQUIPE;
    confirmacao.versao_maior = 1;
    confirmacao.capacidades = CAPACIDADE_SATELITE_PAINEL_WEB;
    confirmacao.crc16 = protocolo_telemetria_calcular_crc_confirmacao(&confirmacao);
    TEST_ASSERT_TRUE(protocolo_telemetria_confirmacao_valida(&confirmacao));
    confirmacao.sequencia++;
    TEST_ASSERT_FALSE(protocolo_telemetria_confirmacao_valida(&confirmacao));
}

void testar_classificacao_de_sequencia() {
    uint16_t perdidos = 0;
    TEST_ASSERT_EQUAL(SEQUENCIA_TELEMETRIA_INICIAL,
        protocolo_telemetria_classificar_sequencia(false, 0, 20, &perdidos));
    TEST_ASSERT_EQUAL(SEQUENCIA_TELEMETRIA_CONTINUA,
        protocolo_telemetria_classificar_sequencia(true, 20, 21, &perdidos));
    TEST_ASSERT_EQUAL(SEQUENCIA_TELEMETRIA_COM_PERDA,
        protocolo_telemetria_classificar_sequencia(true, 21, 25, &perdidos));
    TEST_ASSERT_EQUAL_UINT16(3, perdidos);
    TEST_ASSERT_EQUAL(SEQUENCIA_TELEMETRIA_DUPLICADA,
        protocolo_telemetria_classificar_sequencia(true, 25, 25, &perdidos));
    TEST_ASSERT_EQUAL(SEQUENCIA_TELEMETRIA_FORA_DE_ORDEM,
        protocolo_telemetria_classificar_sequencia(true, 25, 24, &perdidos));
    TEST_ASSERT_EQUAL(SEQUENCIA_TELEMETRIA_CONTINUA,
        protocolo_telemetria_classificar_sequencia(true, UINT16_MAX, 0, &perdidos));
}

void testar_fluxo_incompleto_e_recuperacao() {
    const pacote_telemetria_t pacote = criar_pacote_valido(44);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&pacote);
    static parser_enlace_uart_t parser;
    std::memset(&parser, 0, sizeof(parser));
    protocolo_enlace_uart_inicializar(
        &parser, TELEMETRIA_INICIO_2, TAMANHO_PACOTE_TELEMETRIA);
    ResultadoFluxo resultado{};
    alimentar(parser, resultado, bytes, 40);
    TEST_ASSERT_EQUAL_UINT32(0, resultado.pacotes_validos);
    alimentar(parser, resultado, &bytes[40], sizeof(pacote) - 40);
    TEST_ASSERT_EQUAL_UINT32(1, resultado.pacotes_validos);

    static parser_enlace_uart_t recuperacao;
    std::memset(&recuperacao, 0, sizeof(recuperacao));
    protocolo_enlace_uart_inicializar(
        &recuperacao, TELEMETRIA_INICIO_2, TAMANHO_PACOTE_TELEMETRIA);
    ResultadoFluxo resultado_recuperacao{};
    uint8_t inicio_corrompido[20];
    std::memcpy(inicio_corrompido, bytes, sizeof(inicio_corrompido));
    inicio_corrompido[10] ^= 0x80;
    alimentar(recuperacao, resultado_recuperacao, inicio_corrompido,
              sizeof(inicio_corrompido));
    alimentar(recuperacao, resultado_recuperacao, bytes, sizeof(pacote));
    TEST_ASSERT_EQUAL_UINT32(1, resultado_recuperacao.pacotes_validos);
    TEST_ASSERT_EQUAL_UINT32(1, resultado_recuperacao.pacotes_invalidos);
}

void testar_recuperacao_apos_perda_de_byte() {
    const pacote_telemetria_t pacote = criar_pacote_valido(45);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&pacote);
    static parser_enlace_uart_t parser;
    std::memset(&parser, 0, sizeof(parser));
    protocolo_enlace_uart_inicializar(
        &parser, TELEMETRIA_INICIO_2, TAMANHO_PACOTE_TELEMETRIA);
    ResultadoFluxo resultado{};

    alimentar(parser, resultado, bytes, 50);
    alimentar(parser, resultado, &bytes[51], sizeof(pacote) - 51);
    alimentar(parser, resultado, bytes, sizeof(pacote));

    TEST_ASSERT_EQUAL_UINT32(1, resultado.pacotes_validos);
    TEST_ASSERT_EQUAL_UINT32(1, resultado.pacotes_invalidos);
}

void testar_quadro_ota_maximo_e_crc32() {
    static uint8_t carga[OTA_SATELITE_TAMANHO_MAXIMO_CARGA];
    static uint8_t quadro[OTA_SATELITE_TAMANHO_MAXIMO_QUADRO];
    for (size_t indice = 0; indice < sizeof(carga); ++indice) {
        carga[indice] = static_cast<uint8_t>((indice * 37u) & 0xFFu);
    }

    const size_t tamanho = protocolo_ota_satelites_montar_quadro(
        quadro, sizeof(quadro), OTA_SATELITE_TIPO_DADOS, 0x12345678u, 7u,
        6u * OTA_SATELITE_TAMANHO_MAXIMO_CARGA, carga, sizeof(carga));
    TEST_ASSERT_EQUAL_UINT32(OTA_SATELITE_TAMANHO_MAXIMO_QUADRO, tamanho);
    TEST_ASSERT_TRUE(protocolo_ota_satelites_validar_quadro(quadro, tamanho));

    static parser_enlace_uart_t parser;
    std::memset(&parser, 0, sizeof(parser));
    protocolo_enlace_uart_inicializar(
        &parser, TELEMETRIA_INICIO_2, TAMANHO_PACOTE_TELEMETRIA);
    resultado_parser_enlace_uart_t evento = ENLACE_UART_AGUARDANDO;
    for (size_t indice = 0; indice < tamanho; ++indice) {
        evento = protocolo_enlace_uart_processar_byte(&parser, quadro[indice]);
    }
    TEST_ASSERT_EQUAL(ENLACE_UART_OTA_COMPLETA, evento);

    quadro[OTA_SATELITE_TAMANHO_CABECALHO + 31u] ^= 0x80u;
    TEST_ASSERT_FALSE(protocolo_ota_satelites_validar_quadro(quadro, tamanho));
}

extern "C" void app_main() {
    UNITY_BEGIN();
    RUN_TEST(testar_pacote_valido_e_tamanho);
    RUN_TEST(testar_crc_detecta_corrupcao);
    RUN_TEST(testar_limites_de_conteudo);
    RUN_TEST(testar_confirmacao_e_corrupcao);
    RUN_TEST(testar_classificacao_de_sequencia);
    RUN_TEST(testar_fluxo_incompleto_e_recuperacao);
    RUN_TEST(testar_recuperacao_apos_perda_de_byte);
    RUN_TEST(testar_quadro_ota_maximo_e_crc32);
    UNITY_END();
}
