#include <cstring>

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
    pacote.crc16 = protocolo_telemetria_calcular_crc16(&pacote);
    return pacote;
}

struct ParserTeste {
    uint8_t quadro[TAMANHO_PACOTE_TELEMETRIA]{};
    size_t quantidade = 0;
    unsigned pacotes_validos = 0;
};

void reaproveitar_inicio(ParserTeste& parser) {
    for (size_t indice = 1; indice + 1 < sizeof(parser.quadro); ++indice) {
        if (parser.quadro[indice] == TELEMETRIA_INICIO_1 &&
            parser.quadro[indice + 1] == TELEMETRIA_INICIO_2) {
            parser.quantidade = sizeof(parser.quadro) - indice;
            std::memmove(parser.quadro, &parser.quadro[indice], parser.quantidade);
            return;
        }
    }
    parser.quantidade = 0;
}

void alimentar(ParserTeste& parser, const uint8_t* bytes, size_t tamanho) {
    for (size_t indice = 0; indice < tamanho; ++indice) {
        const uint8_t byte = bytes[indice];
        if (parser.quantidade == 0) {
            if (byte == TELEMETRIA_INICIO_1) parser.quadro[parser.quantidade++] = byte;
            continue;
        }
        if (parser.quantidade == 1) {
            if (byte == TELEMETRIA_INICIO_2) parser.quadro[parser.quantidade++] = byte;
            else if (byte != TELEMETRIA_INICIO_1) parser.quantidade = 0;
            continue;
        }
        parser.quadro[parser.quantidade++] = byte;
        if (parser.quantidade < sizeof(parser.quadro)) continue;
        pacote_telemetria_t pacote{};
        std::memcpy(&pacote, parser.quadro, sizeof(pacote));
        if (protocolo_telemetria_pacote_valido(&pacote) &&
            protocolo_telemetria_conteudo_valido(&pacote)) {
            parser.pacotes_validos++;
            parser.quantidade = 0;
        } else {
            reaproveitar_inicio(parser);
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
    ParserTeste parser{};
    alimentar(parser, bytes, 40);
    TEST_ASSERT_EQUAL_UINT32(0, parser.pacotes_validos);
    alimentar(parser, &bytes[40], sizeof(pacote) - 40);
    TEST_ASSERT_EQUAL_UINT32(1, parser.pacotes_validos);

    ParserTeste recuperacao{};
    uint8_t inicio_corrompido[20];
    std::memcpy(inicio_corrompido, bytes, sizeof(inicio_corrompido));
    inicio_corrompido[10] ^= 0x80;
    alimentar(recuperacao, inicio_corrompido, sizeof(inicio_corrompido));
    alimentar(recuperacao, bytes, sizeof(pacote));
    TEST_ASSERT_EQUAL_UINT32(1, recuperacao.pacotes_validos);
}

extern "C" void app_main() {
    UNITY_BEGIN();
    RUN_TEST(testar_pacote_valido_e_tamanho);
    RUN_TEST(testar_crc_detecta_corrupcao);
    RUN_TEST(testar_limites_de_conteudo);
    RUN_TEST(testar_confirmacao_e_corrupcao);
    RUN_TEST(testar_classificacao_de_sequencia);
    RUN_TEST(testar_fluxo_incompleto_e_recuperacao);
    UNITY_END();
}
