// Declara o retrato desacoplado de saúde e diagnóstico do sistema.
#pragma once

#include <cstdint>

#include "dados_telemetria_veiculo.h"
#include "esp_err.h"

enum class EstadoSaudeSistema : uint8_t {
    Saudavel,
    Recuperando,
    Atencao,
    Falha,
};

enum CausaSaudeSistema : uint32_t {
    CausaSaudeNenhuma = 0,
    CausaSaudeHeapCritico = 1u << 0,
    CausaSaudePilhaCritica = 1u << 1,
    CausaSaudeSensor = 1u << 2,
    CausaSaudeI2c = 1u << 3,
    CausaSaudeUart = 1u << 4,
    CausaSaudeSupervisao = 1u << 5,
    CausaSaudeEquipeSemResposta = 1u << 6,
    CausaSaudeVisitantesSemResposta = 1u << 7,
    CausaSaudeWifiFraco = 1u << 8,
    CausaSaudeOtaRecente = 1u << 9,
    CausaSaudePersistencia = 1u << 10,
};

struct ResumoSaudeSistema {
    EstadoSaudeSistema estado = EstadoSaudeSistema::Saudavel;
    uint64_t tempo_ativo_ms = 0;
    uint32_t heap_livre = 0;
    uint32_t menor_heap_livre = 0;
    uint32_t bytes_pilha_livres = 0;
    uint32_t heap_interno_livre = 0;
    uint32_t maior_bloco_heap_interno = 0;
    uint32_t motivo_ultimo_reset = 0;
    uint32_t erros_sensores = 0;
    uint32_t erros_i2c = 0;
    uint32_t erros_uart = 0;
    uint32_t descartes_uart = 0;
    uint32_t falhas_ota = 0;
    bool wifi_ativo = false;
    bool wifi_conectado = false;
    int8_t wifi_rssi_dbm = 0;
    bool equipe_respondendo = false;
    bool visitantes_respondendo = false;
    uint32_t latencia_equipe_ms = 0;
    uint32_t latencia_visitantes_ms = 0;
    uint32_t falhas_supervisao = 0;
    uint32_t causas_ativas = CausaSaudeNenhuma;
};

enum class SensorDiagnosticado : uint8_t {
    Adc,
    Mpu6050,
    Encoder,
};

struct RegistroErroDiagnostico {
    uint32_t total = 0;
    uint32_t consecutivos = 0;
    uint64_t primeira_ocorrencia_ms = 0;
    uint64_t ultima_ocorrencia_ms = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

struct EstatisticasSensoresDiagnostico {
    RegistroErroDiagnostico adc{};
    RegistroErroDiagnostico mpu6050{};
    RegistroErroDiagnostico encoder{};
    RegistroErroDiagnostico persistencia_odometro{};
};

struct DiagnosticoTelemetria {
    uint16_t ultima_sequencia = 0;
    uint32_t pacotes_gerados = 0;
    uint32_t pacotes_com_falha = 0;
    uint16_t ultimo_crc16 = 0;
};

struct DiagnosticoI2c {
    uint32_t operacoes_ok = 0;
    uint32_t erros = 0;
    uint32_t sondagens_ok = 0;
    uint32_t enderecos_ausentes = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

struct DiagnosticoEncoder {
    bool inicializado = false;
    int pino_a = -1;
    int pino_b = -1;
    uint32_t contagens_por_giro = 0;
    uint64_t contagem_atual = 0;
    uint32_t transbordamentos_compensados = 0;
    uint32_t leituras_ok = 0;
    uint32_t erros_leitura = 0;
};

struct DiagnosticoRodaOdometro {
    uint64_t contagens_acumuladas = 0;
    double percurso_m = 0.0;
    float velocidade_kmh = 0.0f;
    double metros_por_contagem = 0.0;
};

struct DiagnosticoOdometro {
    DiagnosticoRodaOdometro roda_esquerda{};
    DiagnosticoRodaOdometro roda_direita{};
    double percurso_m = 0.0;
    double odometro_m = 0.0;
    float velocidade_kmh = 0.0f;
    uint32_t erros_persistencia = 0;
    uint32_t erros_validacao = 0;
    uint32_t divergencias_detectadas = 0;
    bool rodas_coerentes = true;
    bool persistencia_disponivel = false;
    bool persistencia_pendente = false;
};

struct DiagnosticoWifi {
    bool radio_ativo = false;
    bool conectado = false;
    char rede_ativa[33] = {};
    int8_t rssi_dbm = 0;
    uint8_t canal = 0;
    int32_t ultimo_motivo_desconexao = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

struct DiagnosticoSupervisao {
    uint32_t idade_ultimo_pulso_ms = 0;
    uint32_t falhas_detectadas = 0;
    uint32_t recuperacoes_ok = 0;
};

// Agregado centralizado para interfaces de observabilidade. Terminal e
// relatórios formatam estes dados sem repetir a composição em cada saída.
struct RetratoDiagnosticoSistema {
    uint64_t coletado_em_ms = 0;
    DadosTelemetriaVeiculo telemetria_atual{};
    DiagnosticoTelemetria telemetria{};
    EstatisticasSensoresDiagnostico sensores{};
    ResumoSaudeSistema saude{};
    DiagnosticoI2c i2c{};
    DiagnosticoSupervisao supervisao_telemetria{};
    DiagnosticoSupervisao supervisao_equipe{};
    DiagnosticoSupervisao supervisao_visitantes{};
    DiagnosticoEncoder encoder_esquerdo{};
    DiagnosticoEncoder encoder_direito{};
    DiagnosticoOdometro odometro{};
    DiagnosticoWifi wifi{};
};

esp_err_t servico_diagnostico_iniciar();
esp_err_t servico_diagnostico_registrar_resultado(
    SensorDiagnosticado sensor, esp_err_t resultado);
EstatisticasSensoresDiagnostico servico_diagnostico_obter_sensores();
ResumoSaudeSistema servico_diagnostico_obter_resumo();
RetratoDiagnosticoSistema servico_diagnostico_obter_retrato();
const char* servico_diagnostico_nome_estado(EstadoSaudeSistema estado);
