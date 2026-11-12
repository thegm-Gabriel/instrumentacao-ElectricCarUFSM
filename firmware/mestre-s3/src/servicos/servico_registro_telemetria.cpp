// Coordena a fila de amostras, arquivos CSV recuperáveis e upload HTTPS em lote.
#include "nucleo/configuracao_tarefas.h"
#include "servicos/servico_registro_telemetria.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_cartao_sd.h"
#include "nucleo/configuracao_registro_telemetria.h"
#include "servicos/servico_wifi.h"
#include "servicos/servico_diagnostico.h"
#include "servicos/servico_supervisao.h"
#include "servicos/servico_odometro.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_relatorios.h"
#include "servicos/servico_telemetria.h"

namespace {
constexpr char ETIQUETA[] = "registro_telemetria";
constexpr char CABECALHO_CSV[] =
    "tempo_sessao_ms,tensao_adc_bruta,aceleracao_x_bruta,aceleracao_y_bruta,"
    "aceleracao_z_bruta,giroscopio_x_bruto,giroscopio_y_bruto,giroscopio_z_bruto,"
    "aceleracao_x_g,aceleracao_y_g,aceleracao_z_g,aceleracao_x_ms2,"
    "aceleracao_y_ms2,aceleracao_z_ms2,giroscopio_x_graus_s,"
    "giroscopio_y_graus_s,giroscopio_z_graus_s,temperatura_mpu_c,mpu_ativo,"
    "mpu_calibrado,eixo_longitudinal_compativel,eixo_longitudinal_mpu,"
    "sentido_aceleracao,marcha,velocidade_kmh,aceleracao_ms2,aceleracao_g,"
    "acelerador_percentual,freio_percentual,tensao_pacote_v,corrente_a,"
    "carga_percentual,celula_1_v,celula_2_v,celula_3_v,celula_4_v,latitude,"
    "longitude,rumo_graus,distancia_1_cm,distancia_2_cm,distancia_3_cm,"
    "distancia_4_cm,distancia_5_cm,distancia_6_cm,distancia_7_cm,"
    "distancia_8_cm,potencia_w,autonomia_km,percurso_km,odometro_km,simulado\n";

enum class TipoComandoRegistro : uint8_t { Amostra, FecharArquivo };

struct ComandoRegistro {
    TipoComandoRegistro tipo = TipoComandoRegistro::Amostra;
    uint32_t tempo_sessao_ms = 0;
    DadosTelemetriaVeiculo dados{};
};

struct RespostaHttp {
    char texto[160]{};
    char redirecionamento[2048]{};
    size_t tamanho = 0;
};

struct ResultadoSincronizacao {
    uint32_t arquivos_total = 0;
    uint32_t arquivos_enviados = 0;
    uint32_t bytes_enviados = 0;
};

QueueHandle_t fila = nullptr;
SemaphoreHandle_t arquivo_pronto_para_upload = nullptr;
TaskHandle_t tarefa_registro_handle = nullptr;
TaskHandle_t tarefa_upload_handle = nullptr;
TaskHandle_t tarefa_historico_handle = nullptr;
FILE* arquivo_atual = nullptr;
char caminho_temporario[192]{};
char nome_final_atual[96]{};
uint32_t sessao = 0;
uint32_t sequencia_arquivo = 0;
uint32_t inicio_arquivo_ms = 0;
uint32_t linhas_sem_sincronizar = 0;
uint32_t proximo_registro_ms = 0;
EstadoRegistroTelemetria estado{};
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;
esp_err_t erro_preparacao_upload = ESP_OK;

// Somente tarefa_registro acessa o arquivo de diagnóstico. Sem hooks de printf,
// alocações por mensagem ou gravações SD em callbacks de rede/interrupção.
char caminho_log[192]{};
uint32_t numero_log = 0;
uint32_t ultimo_log_ms = 0;
uint32_t ultimo_log_tarefas_ms = 0;
uint32_t janela_log_falha_ate_ms = 0;
uint32_t janela_log_grave_ate_ms = 0;
uint32_t repetir_log_ms = 0;
size_t bytes_log_sessao = 0;
bool log_limite_avisado = false;
ResumoSaudeSistema ultimo_log_saude{};
EstatisticasSensoresDiagnostico ultimos_log_sensores{};
EstatisticasTelemetria ultimo_log_telemetria{};
EstatisticasLinkTelemetria ultimo_log_equipe{};
EstatisticasLinkTelemetria ultimo_log_visitantes{};
ResumoWifi ultimo_log_wifi{};
SituacaoOta ultimo_log_ota{};
EstadoModuloSupervisionado ultimos_log_modulos[
    static_cast<unsigned>(ModuloSupervisionado::Quantidade)]{};
uint32_t ultimo_log_sincronizacoes = 0;
uint32_t ultimo_log_descartes = 0;
esp_err_t ultimo_log_erro_csv = ESP_OK;
DiagnosticoTarefas* tarefas_log = nullptr;
// Histórico curto em RAM; o escritor SD nunca roda na tarefa de telemetria.
struct AmostraHistorico {
    uint64_t ordem = 0;
    int64_t utc = 0;
    uint32_t ms = 0;
    uint32_t causas = 0;
    uint32_t heap_interno = 0;
    uint32_t bloco_interno = 0;
    uint32_t psram = 0;
    uint32_t pilha_minima = 0;
    uint32_t erros_sensores = 0;
    uint32_t erros_i2c = 0;
    uint32_t erros_uart = 0;
    uint32_t falhas_ota = 0;
    uint32_t falhas_supervisao = 0;
    uint32_t falhas_telemetria = 0;
    uint32_t descartes_csv = 0;
    uint32_t wifi_desconexoes = 0;
    uint32_t link_equipe_invalidas = 0;
    uint32_t link_visitantes_invalidas = 0;
    int32_t wifi_erro = 0;
    uint32_t sat_equipe_idade_ms = 0;
    uint32_t sat_visitantes_idade_ms = 0;
    uint16_t sat_equipe_rtt_ms = 0;
    uint16_t sat_visitantes_rtt_ms = 0;
    uint8_t saude = 0;
    uint8_t sat_equipe_responde = 0;
    uint8_t sat_visitantes_responde = 0;
    uint8_t tarefas_atrasadas = 0;
    char tarefa_menor_pilha[24]{};
};
constexpr size_t AMOSTRAS_HISTORICO_PSRAM = 120;
constexpr size_t AMOSTRAS_HISTORICO_INTERNO = 30;
AmostraHistorico* amostras_historico = nullptr;
AmostraHistorico* historico_pendente = nullptr;
size_t capacidade_historico = 0;
size_t inicio_historico = 0;
size_t quantidade_historico = 0;
size_t quantidade_pendente = 0;
uint64_t ordem_amostra = 0;
uint64_t ordem_incidente_pendente = 0;
uint32_t incidentes_nao_retidos = 0;
uint32_t amostras_ram_descartadas = 0;
uint64_t ordem_historico_tentativa = 0;
bool historico_parcial_possivel = false;
SemaphoreHandle_t mutex_historico = nullptr;
uint32_t ultimo_backup_ms = 0;
uint32_t ultima_tentativa_backup_falha_ms = 0;
bool aguarda_nova_tentativa_backup = false;
uint32_t sequencia_backup = 0;
uint64_t ultimo_odometro_backup_mm = UINT64_MAX;
esp_err_t criar_diretorio(const char* caminho);

void iniciar_sequencia_logs() {
    DIR* diretorio = opendir(configuracao::DIRETORIO_LOG_ESP);
    if (diretorio == nullptr) return;
    uint32_t maior = 0;
    while (dirent* entrada = readdir(diretorio)) {
        const char* nome = entrada->d_name;
        if (std::strlen(nome) != 16 || std::strncmp(nome, "esp_", 4) != 0 ||
            std::strcmp(nome + 12, ".log") != 0) continue;
        uint32_t numero = 0;
        bool valido = true;
        for (size_t i = 4; i < 12; ++i) {
            if (nome[i] < '0' || nome[i] > '9') {
                valido = false;
                break;
            }
            numero = numero * 10U + static_cast<uint32_t>(nome[i] - '0');
        }
        if (valido && numero > maior) maior = numero;
    }
    closedir(diretorio);
    numero_log = maior + 1U;
    if (numero_log == 0) numero_log = 1;
}

void acertar_relogio_http_interno(const char* valor) {
    if (valor == nullptr || std::time(nullptr) >= 1704067200) return;
    char dia_semana[4]{};
    char mes[4]{};
    char zona[4]{};
    int dia = 0, ano = 0, hora = 0, minuto = 0, segundo = 0;
    if (std::sscanf(valor, "%3s, %d %3s %d %d:%d:%d %3s", dia_semana,
                    &dia, mes, &ano, &hora, &minuto, &segundo, zona) != 8 ||
        std::strcmp(zona, "GMT") != 0 || ano < 2024 || ano > 2100 ||
        dia < 1 || dia > 31 || hora > 23 || minuto > 59 || segundo > 60) return;
    const char* meses[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int indice_mes = -1;
    for (int i = 0; i < 12; ++i) {
        if (std::strcmp(mes, meses[i]) == 0) { indice_mes = i; break; }
    }
    if (indice_mes < 0) return;
    // Converte a data UTC sem depender de timegm(), ausente em algumas
    // versões da libc fornecida com o ESP-IDF.
    int ano_civil = ano;
    const unsigned mes_civil = static_cast<unsigned>(indice_mes + 1);
    ano_civil -= mes_civil <= 2;
    const int era = ano_civil / 400;
    const unsigned ano_da_era = static_cast<unsigned>(ano_civil - era * 400);
    const unsigned mes_ajustado = mes_civil > 2 ? mes_civil - 3U : mes_civil + 9U;
    const unsigned dia_do_ano =
        (153U * mes_ajustado + 2U) / 5U +
        static_cast<unsigned>(dia) - 1U;
    const unsigned ano_da_era_dias = ano_da_era * 365U + ano_da_era / 4U -
        ano_da_era / 100U + dia_do_ano;
    const int64_t dias_desde_epoch =
        static_cast<int64_t>(era) * 146097 + ano_da_era_dias - 719468;
    const time_t epoch = static_cast<time_t>(
        dias_desde_epoch * 86400 + hora * 3600 + minuto * 60 + segundo);
    if (epoch < 1704067200) return;
    timeval instante{};
    instante.tv_sec = epoch;
    if (settimeofday(&instante, nullptr) == 0) {
        ESP_LOGI(ETIQUETA, "Relógio UTC sincronizado com cabeçalho HTTPS Date");
    }
}

uint32_t checksum_backup(const char* dados, size_t tamanho) {
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < tamanho; ++i) {
        hash = (hash ^ static_cast<unsigned char>(dados[i])) * 16777619U;
    }
    return hash;
}

struct BackupConfiguracao {
    uint32_t sequencia = 0;
    uint64_t odometro_mm = 0;
    PoliticaConexaoWifi politica_wifi{};
    bool conexao_automatica_wifi = false;
    uint32_t intervalo_ota_min = 0;
    uint32_t intervalos_relatorios_ms[4]{};
    bool relatorios_pausados = false;
};

bool ler_campo_backup(const char* conteudo, const char* chave, uint32_t* destino) {
    char busca[48]{};
    if (std::snprintf(busca, sizeof(busca), "\n%s=", chave) >=
        static_cast<int>(sizeof(busca))) return false;
    const char* campo = std::strstr(conteudo, busca);
    if (campo == nullptr) return false;
    campo += std::strlen(busca);
    if (*campo < '0' || *campo > '9') return false;
    char* fim = nullptr;
    errno = 0;
    const unsigned long long valor = std::strtoull(campo, &fim, 10);
    if (errno != 0 || fim == campo || *fim != '\n' || valor > UINT32_MAX) {
        return false;
    }
    *destino = static_cast<uint32_t>(valor);
    return true;
}

bool ler_backup(const char* caminho, BackupConfiguracao* backup) {
    if (backup == nullptr) return false;
    FILE* arquivo = std::fopen(caminho, "rb");
    if (arquivo == nullptr) return false;
    char conteudo[1024]{};
    const size_t lidos = std::fread(conteudo, 1, sizeof(conteudo) - 1, arquivo);
    const bool completo = std::feof(arquivo) != 0;
    std::fclose(arquivo);
    if (!completo || lidos == 0) return false;
    conteudo[lidos] = '\0';
    char* checksum = std::strstr(conteudo, "checksum=");
    unsigned esperado = 0;
    unsigned numero = 0;
    unsigned long long total = 0;
    if (checksum == nullptr ||
        std::sscanf(checksum, "checksum=%8x", &esperado) != 1 ||
        std::sscanf(conteudo, "formato=1\nsequencia=%u\nodometro_mm=%llu\n",
                    &numero, &total) != 2 ||
        total > 1000000000000ULL ||
        checksum_backup(conteudo, static_cast<size_t>(checksum - conteudo)) != esperado) {
        return false;
    }
    uint32_t tentativas = 0;
    uint32_t automatico = 0;
    uint32_t pausados = 0;
    if (!ler_campo_backup(conteudo, "wifi_automatico", &automatico) ||
        automatico > 1 ||
        !ler_campo_backup(conteudo, "wifi_tentativas", &tentativas) ||
        tentativas > UINT8_MAX ||
        !ler_campo_backup(conteudo, "wifi_tempo_tentativa_ms",
                          &backup->politica_wifi.tempo_por_tentativa_ms) ||
        !ler_campo_backup(conteudo, "wifi_intervalo_ms",
                          &backup->politica_wifi.intervalo_entre_tentativas_ms) ||
        !ler_campo_backup(conteudo, "wifi_limite_sessao_ms",
                          &backup->politica_wifi.tempo_limite_sessao_ms) ||
        !ler_campo_backup(conteudo, "ota_intervalo_min",
                          &backup->intervalo_ota_min) ||
        !ler_campo_backup(conteudo, "relatorio_telemetria_ms",
                          &backup->intervalos_relatorios_ms[0]) ||
        !ler_campo_backup(conteudo, "relatorio_uart_ms",
                          &backup->intervalos_relatorios_ms[1]) ||
        !ler_campo_backup(conteudo, "relatorio_sistema_ms",
                          &backup->intervalos_relatorios_ms[2]) ||
        !ler_campo_backup(conteudo, "relatorio_ota_ms",
                          &backup->intervalos_relatorios_ms[3]) ||
        !ler_campo_backup(conteudo, "relatorios_pausados", &pausados) ||
        pausados > 1) return false;
    backup->sequencia = numero;
    backup->odometro_mm = static_cast<uint64_t>(total);
    backup->politica_wifi.tentativas_por_rede = static_cast<uint8_t>(tentativas);
    backup->conexao_automatica_wifi = automatico != 0;
    backup->relatorios_pausados = pausados != 0;
    return true;
}

void recuperar_backup_configuracao() {
    BackupConfiguracao principal_backup{};
    BackupConfiguracao temporario_backup{};
    const bool principal = ler_backup(configuracao::ARQUIVO_BACKUP_CONFIGURACAO,
                                      &principal_backup);
    char temporario[96]{};
    std::snprintf(temporario, sizeof(temporario), "%s.tmp",
                  configuracao::ARQUIVO_BACKUP_CONFIGURACAO);
    const bool pendente = ler_backup(temporario, &temporario_backup);
    if (!principal && !pendente) return;
    const bool usar_temporario = pendente &&
        (!principal || static_cast<int32_t>(temporario_backup.sequencia -
                                           principal_backup.sequencia) > 0);
    const BackupConfiguracao& escolhido = usar_temporario
        ? temporario_backup : principal_backup;
    sequencia_backup = escolhido.sequencia;
    ultimo_odometro_backup_mm = escolhido.odometro_mm;
    const esp_err_t erro = servico_odometro_restaurar_se_ausente(
        escolhido.odometro_mm);
    if (erro != ESP_OK && erro != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ETIQUETA, "Backup SD encontrado, mas restauração do odômetro falhou: %s",
                 esp_err_to_name(erro));
    }
    const esp_err_t wifi = servico_wifi_restaurar_politica_se_ausente(
        escolhido.politica_wifi, escolhido.conexao_automatica_wifi);
    if (wifi == ESP_OK) {
        ESP_LOGI(ETIQUETA, "Política Wi-Fi restaurada do SD; redes exigem credenciais");
    } else if (wifi != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ETIQUETA, "Política Wi-Fi do SD não restaurada: %s",
                 esp_err_to_name(wifi));
    }
    const esp_err_t ota = servico_ota_restaurar_intervalo_se_ausente(
        escolhido.intervalo_ota_min);
    if (ota == ESP_OK) {
        ESP_LOGI(ETIQUETA, "Intervalo OTA restaurado do SD");
    } else if (ota != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ETIQUETA, "Intervalo OTA do SD não restaurado: %s",
                 esp_err_to_name(ota));
    }
    const esp_err_t relatorios =
        servico_relatorios_restaurar_configuracao_antes_de_iniciar(
            escolhido.intervalos_relatorios_ms, escolhido.relatorios_pausados);
    if (relatorios != ESP_OK && relatorios != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ETIQUETA, "Relatórios do SD não restaurados: %s",
                 esp_err_to_name(relatorios));
    }
    if (usar_temporario) {
        (void)std::remove(configuracao::ARQUIVO_BACKUP_CONFIGURACAO);
        if (std::rename(temporario, configuracao::ARQUIVO_BACKUP_CONFIGURACAO) != 0) {
            ESP_LOGW(ETIQUETA, "Backup temporário válido preservado: errno=%d", errno);
        }
    }
}

void atualizar_backup_configuracao(uint32_t instante) {
    if (ultimo_backup_ms != 0 && instante - ultimo_backup_ms <
            configuracao::INTERVALO_BACKUP_CONFIGURACAO_MS) return;
    // Falhas no SD devem ser repetidas em breve, sem tentar e registrar erro a cada amostra.
    if (aguarda_nova_tentativa_backup &&
        instante - ultima_tentativa_backup_falha_ms < 5000) return;
    const EstadoOdometro odometro = servico_odometro_obter_estado();
    if (!odometro.iniciado || !std::isfinite(odometro.odometro_m) ||
        odometro.odometro_m < 0) return;
    const uint64_t total_mm = static_cast<uint64_t>(
        std::llround(odometro.odometro_m * 1000.0));
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    const SituacaoOta ota = servico_ota_obter_situacao();
    if (ota.intervalo_verificacao_minutos == 0) return;
    aguarda_nova_tentativa_backup = true;
    ultima_tentativa_backup_falha_ms = instante;
    const uint32_t proxima_sequencia_backup = sequencia_backup + 1;
    // Um minuto limita desgaste do SD; o retrato guarda configuração sem senhas.
    char conteudo[1024]{};
    const int tamanho = std::snprintf(conteudo, sizeof(conteudo),
        "formato=1\nsequencia=%" PRIu32 "\nodometro_mm=%" PRIu64 "\n"
        "wifi_secundarias=%u\nwifi_automatico=%u\n"
        "wifi_tentativas=%u\nwifi_tempo_tentativa_ms=%" PRIu32 "\n"
        "wifi_intervalo_ms=%" PRIu32 "\nwifi_limite_sessao_ms=%" PRIu32 "\n"
        "ota_intervalo_min=%" PRIu32 "\n"
        "relatorio_telemetria_ms=%" PRIu32 "\nrelatorio_uart_ms=%" PRIu32 "\n"
        "relatorio_sistema_ms=%" PRIu32 "\nrelatorio_ota_ms=%" PRIu32 "\n"
        "relatorios_pausados=%u\n"
        "registro_intervalo_ms=%" PRIu32 "\nregistro_duracao_arquivo_ms=%" PRIu32 "\n",
        proxima_sequencia_backup, total_mm,
        static_cast<unsigned>(wifi.quantidade_redes_usuario),
        static_cast<unsigned>(wifi.conectar_secundarias_automaticamente),
        static_cast<unsigned>(wifi.politica.tentativas_por_rede),
        wifi.politica.tempo_por_tentativa_ms,
        wifi.politica.intervalo_entre_tentativas_ms,
        wifi.politica.tempo_limite_sessao_ms,
        ota.intervalo_verificacao_minutos,
        servico_relatorios_obter_intervalo(GrupoRelatorio::Telemetria),
        servico_relatorios_obter_intervalo(GrupoRelatorio::Uart),
        servico_relatorios_obter_intervalo(GrupoRelatorio::Sistema),
        servico_relatorios_obter_intervalo(GrupoRelatorio::Ota),
        static_cast<unsigned>(servico_relatorios_estao_pausados()),
        configuracao::INTERVALO_REGISTRO_TELEMETRIA_MS,
        configuracao::DURACAO_ARQUIVO_TELEMETRIA_MS);
    if (tamanho <= 0 || tamanho + 19 >= static_cast<int>(sizeof(conteudo))) return;
    size_t usados = static_cast<size_t>(tamanho);
    auto adicionar_rede = [&](const char* chave, const char* ssid) {
        if (usados + std::strlen(chave) + 34 >= sizeof(conteudo)) return;
        usados += static_cast<size_t>(std::snprintf(conteudo + usados,
                sizeof(conteudo) - usados, "%s=", chave));
        for (size_t i = 0; i < 32 && ssid[i] != '\0'; ++i) {
            const unsigned char c = static_cast<unsigned char>(ssid[i]);
            conteudo[usados++] = c >= 32 && c < 127 && c != '=' ? c : '_';
        }
        conteudo[usados++] = '\n';
        conteudo[usados] = '\0';
    };
    adicionar_rede("wifi_principal", wifi.rede_principal);
    for (unsigned i = 0; i < wifi.quantidade_redes_usuario; ++i) {
        char chave[32]{};
        std::snprintf(chave, sizeof(chave), "wifi_secundaria_%u", i + 1);
        adicionar_rede(chave, wifi.redes_usuario[i]);
    }
    if (usados + 19 >= sizeof(conteudo)) return;
    const uint32_t hash = checksum_backup(conteudo, usados);
    const int final = std::snprintf(conteudo + usados, sizeof(conteudo) - usados,
                                    "checksum=%08" PRIx32 "\n", hash);
    if (final <= 0 || final >= static_cast<int>(sizeof(conteudo) - usados)) return;
    char temporario[96]{};
    std::snprintf(temporario, sizeof(temporario), "%s.tmp",
                  configuracao::ARQUIVO_BACKUP_CONFIGURACAO);
    FILE* arquivo = std::fopen(temporario, "wb");
    if (arquivo == nullptr) {
        ESP_LOGW(ETIQUETA, "Falha ao abrir backup SD: errno=%d", errno);
        return;
    }
    const size_t comprimento = usados + static_cast<size_t>(final);
    const bool falhou = std::fwrite(conteudo, 1, comprimento, arquivo) != comprimento ||
        std::fflush(arquivo) != 0 || fsync(fileno(arquivo)) != 0;
    const bool fechamento_falhou = std::fclose(arquivo) != 0;
    if (falhou || fechamento_falhou) {
        ESP_LOGW(ETIQUETA, "Falha ao gravar backup SD: errno=%d", errno);
        return;
    }
    (void)std::remove(configuracao::ARQUIVO_BACKUP_CONFIGURACAO);
    if (std::rename(temporario, configuracao::ARQUIVO_BACKUP_CONFIGURACAO) != 0) {
        ESP_LOGW(ETIQUETA, "Backup temporário preservado: errno=%d", errno);
        return;
    }
    sequencia_backup = proxima_sequencia_backup;
    aguarda_nova_tentativa_backup = false;
    ultimo_backup_ms = instante;
    if (total_mm != ultimo_odometro_backup_mm) {
        ESP_LOGI(ETIQUETA, "Backup de configuração atualizado: sequência %" PRIu32
                 ", odômetro %.3f km", sequencia_backup, odometro.odometro_m / 1000.0);
        ultimo_odometro_backup_mm = total_mm;
    }
}

esp_err_t preparar_historico_no_arquivo(FILE* arquivo, uint32_t sessao_atual,
                                         uint64_t ordem_incidente,
                                         size_t amostras_esperadas,
                                         bool* ja_gravado) {
    if (arquivo == nullptr || ja_gravado == nullptr) return ESP_ERR_INVALID_ARG;
    *ja_gravado = false;
    if (std::fseek(arquivo, 0, SEEK_END) != 0) return ESP_FAIL;
    const long tamanho_arquivo = std::ftell(arquivo);
    if (tamanho_arquivo < 0) return ESP_FAIL;
    // Um lote tem no máximo 121 linhas curtas. Só a cauda pode conter a
    // tentativa anterior; não varre um log de 8 MiB no caminho de registro.
    constexpr long CAUDA_BUSCA_BYTES = 128L * 1024L;
    const long inicio_busca = tamanho_arquivo > CAUDA_BUSCA_BYTES
        ? tamanho_arquivo - CAUDA_BUSCA_BYTES : 0;
    char linha[512]{};
    if (inicio_busca != 0) {
        if (std::fseek(arquivo, inicio_busca - 1, SEEK_SET) != 0) return ESP_FAIL;
        const int anterior = std::fgetc(arquivo);
        if (anterior == EOF) return ESP_FAIL;
        if (anterior != '\n' &&
            std::fgets(linha, sizeof(linha), arquivo) == nullptr) return ESP_FAIL;
    } else if (std::fseek(arquivo, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    long inicio_incompleto = -1;
    size_t amostras_validas = 0;
    while (true) {
        const long posicao = std::ftell(arquivo);
        if (posicao < 0) return ESP_FAIL;
        if (std::fgets(linha, sizeof(linha), arquivo) == nullptr) break;
        unsigned sessao_lida = 0;
        unsigned long long ordem_lida = 0;
        if (std::sscanf(linha,
                "HIST_INICIO sessao=%8x incidente_ordem=%llu",
                &sessao_lida, &ordem_lida) == 2 &&
            sessao_lida == sessao_atual && ordem_lida == ordem_incidente) {
            inicio_incompleto = posicao;
            amostras_validas = 0;
        } else if (inicio_incompleto >= 0 &&
                   std::strncmp(linha, "HIST ordem=", 11) == 0 &&
                   std::strchr(linha, '\n') != nullptr) {
            amostras_validas++;
        } else if (inicio_incompleto >= 0 &&
                   std::sscanf(linha, "HIST_FIM incidente_ordem=%llu",
                               &ordem_lida) == 1 &&
                   ordem_lida == ordem_incidente) {
            if (amostras_validas == amostras_esperadas) {
                *ja_gravado = true;
                inicio_incompleto = -1;
            }
        }
    }
    if (std::ferror(arquivo)) return ESP_FAIL;
    if (inicio_incompleto >= 0 &&
        ftruncate(fileno(arquivo), static_cast<off_t>(inicio_incompleto)) != 0) {
        return ESP_FAIL;
    }
    return std::fseek(arquivo, 0, SEEK_END) == 0 ? ESP_OK : ESP_FAIL;
}

void registrar_diagnostico_esp() {
    const uint32_t instante = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (repetir_log_ms != 0 && static_cast<int32_t>(instante - repetir_log_ms) < 0) return;
    bool ha_historico_pendente = historico_parcial_possivel;
    bool historico_grave_pendente = false;
    if (mutex_historico != nullptr &&
        xSemaphoreTake(mutex_historico, pdMS_TO_TICKS(10)) == pdTRUE) {
        ha_historico_pendente = quantidade_pendente != 0 ||
            incidentes_nao_retidos != 0;
        if (quantidade_pendente != 0) {
            const auto& incidente = historico_pendente[quantidade_pendente - 1];
            historico_grave_pendente =
                incidente.saude == static_cast<uint8_t>(EstadoSaudeSistema::Falha) ||
                (incidente.causas & (CausaSaudeHeapCritico |
                                     CausaSaudePilhaCritica |
                                     CausaSaudeSupervisao)) != 0 ||
                incidente.tarefas_atrasadas != 0;
        }
        xSemaphoreGive(mutex_historico);
    }
    if (bytes_log_sessao >= configuracao::LIMITE_LOG_ESP_POR_SESSAO &&
        !ha_historico_pendente) {
        if (!log_limite_avisado) ESP_LOGW(ETIQUETA, "Limite do log ESP nesta sessão atingido; CSV continua ativo");
        log_limite_avisado = true;
        return;
    }
    const auto saude = servico_diagnostico_obter_resumo();
    if (!saude.valido) return;
    const auto registro = servico_registro_telemetria_obter_estado();
    const auto sensores = servico_diagnostico_obter_sensores();
    const auto telemetria = servico_telemetria_obter_estatisticas();
    const auto equipe = servico_telemetria_obter_estatisticas_link(
        SateliteTelemetria::Equipe);
    const auto visitantes = servico_telemetria_obter_estatisticas_link(
        SateliteTelemetria::Visitantes);
    const auto wifi = servico_wifi_obter_resumo();
    const auto ota = servico_ota_obter_situacao();
    EstadoModuloSupervisionado modulos[
        static_cast<unsigned>(ModuloSupervisionado::Quantidade)]{};
    bool falha_supervisao_nova = false;
    bool modulo_mudou = false;
    bool modulo_grave = false;
    for (unsigned i = 0; i < static_cast<unsigned>(ModuloSupervisionado::Quantidade); ++i) {
        modulos[i] = servico_supervisao_obter_estado(static_cast<ModuloSupervisionado>(i));
        const auto& anterior = ultimos_log_modulos[i];
        falha_supervisao_nova |= modulos[i].falhas_detectadas > anterior.falhas_detectadas ||
            modulos[i].recuperacoes_com_falha > anterior.recuperacoes_com_falha ||
            (modulos[i].ultimo_resultado != ESP_OK &&
             modulos[i].ultimo_resultado != anterior.ultimo_resultado) ||
            (modulos[i].atrasado && !anterior.atrasado);
        modulo_mudou |= modulos[i].atrasado != anterior.atrasado ||
            modulos[i].em_recuperacao != anterior.em_recuperacao ||
            modulos[i].ultimo_resultado != anterior.ultimo_resultado;
        modulo_grave |= modulos[i].atrasado || modulos[i].em_recuperacao;
    }
    const bool falha_sensor_nova =
        sensores.adc.total > ultimos_log_sensores.adc.total ||
        sensores.mpu6050.total > ultimos_log_sensores.mpu6050.total ||
        sensores.ina228.total > ultimos_log_sensores.ina228.total ||
        sensores.encoder.total > ultimos_log_sensores.encoder.total ||
        sensores.persistencia_odometro.total >
            ultimos_log_sensores.persistencia_odometro.total;
    const bool falha_link_nova =
        telemetria.pacotes_com_falha > ultimo_log_telemetria.pacotes_com_falha ||
        equipe.confirmacoes_invalidas > ultimo_log_equipe.confirmacoes_invalidas ||
        visitantes.confirmacoes_invalidas > ultimo_log_visitantes.confirmacoes_invalidas ||
        (ultimo_log_equipe.respondeu && !equipe.respondeu) ||
        (ultimo_log_visitantes.respondeu && !visitantes.respondeu);
    const bool falha_wifi_nova =
        wifi.desconexoes_inesperadas > ultimo_log_wifi.desconexoes_inesperadas ||
        (wifi.ultimo_erro != ESP_OK && wifi.ultimo_erro != ultimo_log_wifi.ultimo_erro) ||
        (ultimo_log_wifi.conectado && !wifi.conectado && wifi.sessao_em_andamento);
    const bool falha_ota_nova =
        ota.falhas > ultimo_log_ota.falhas ||
        (ota.ultimo_erro != ESP_OK && ota.ultimo_erro != ultimo_log_ota.ultimo_erro) ||
        (ota.estado == EstadoServicoOta::Falha &&
         ultimo_log_ota.estado != EstadoServicoOta::Falha) ||
        (ota.equipe.estado == EstadoOtaSatelite::Falha &&
         ultimo_log_ota.equipe.estado != EstadoOtaSatelite::Falha) ||
        (ota.equipe.ultimo_erro != ESP_OK &&
         ota.equipe.ultimo_erro != ultimo_log_ota.equipe.ultimo_erro) ||
        (ota.visitantes.estado == EstadoOtaSatelite::Falha &&
         ultimo_log_ota.visitantes.estado != EstadoOtaSatelite::Falha) ||
        (ota.visitantes.ultimo_erro != ESP_OK &&
         ota.visitantes.ultimo_erro != ultimo_log_ota.visitantes.ultimo_erro);
    const bool falha_registro_nova =
        registro.amostras_descartadas > ultimo_log_descartes ||
        (registro.ultimo_erro != ESP_OK && registro.ultimo_erro != ultimo_log_erro_csv) ||
        (registro.sincronizacoes_finalizadas > ultimo_log_sincronizacoes &&
         registro.ultimo_resultado_sincronizacao != ESP_OK);
    const bool falha_saude_nova =
        (saude.causas_ativas & ~ultimo_log_saude.causas_ativas) != 0 ||
        saude.erros_sensores > ultimo_log_saude.erros_sensores ||
        saude.erros_i2c > ultimo_log_saude.erros_i2c ||
        saude.erros_uart > ultimo_log_saude.erros_uart ||
        saude.descartes_uart > ultimo_log_saude.descartes_uart ||
        saude.falhas_ota > ultimo_log_saude.falhas_ota ||
        saude.falhas_supervisao > ultimo_log_saude.falhas_supervisao;
    const bool falha_nova = falha_saude_nova || falha_sensor_nova ||
        falha_link_nova || falha_wifi_nova || falha_ota_nova ||
        falha_supervisao_nova || falha_registro_nova;
    const uint32_t causas_graves = CausaSaudeHeapCritico |
        CausaSaudePilhaCritica | CausaSaudeSupervisao;
    const bool grave = saude.estado == EstadoSaudeSistema::Falha ||
        (saude.causas_ativas & causas_graves) != 0 || modulo_grave;
    if (falha_nova) {
        const uint32_t duracao = grave
            ? configuracao::JANELA_LOG_FALHA_GRAVE_MS
            : configuracao::JANELA_LOG_FALHA_MS;
        if (grave) janela_log_grave_ate_ms = instante + duracao;
        else janela_log_falha_ate_ms = instante + duracao;
    }
    if (ha_historico_pendente) {
        if (historico_grave_pendente) {
            janela_log_grave_ate_ms = instante +
                configuracao::JANELA_LOG_FALHA_GRAVE_MS;
        } else {
            janela_log_falha_ate_ms = instante +
                configuracao::JANELA_LOG_FALHA_MS;
        }
    }
    const bool janela_grave = janela_log_grave_ate_ms != 0 &&
        static_cast<int32_t>(instante - janela_log_grave_ate_ms) < 0;
    const bool janela_falha = janela_log_falha_ate_ms != 0 &&
        static_cast<int32_t>(instante - janela_log_falha_ate_ms) < 0;
    const uint32_t intervalo = janela_grave
        ? configuracao::INTERVALO_LOG_FALHA_GRAVE_MS
        : janela_falha ? configuracao::INTERVALO_LOG_FALHA_MS
                        : configuracao::INTERVALO_LOG_ESP_MS;
    // Limita rajadas contínuas a uma gravação por segundo, mesmo com novos erros a 4 Hz.
    if (ultimo_log_ms != 0 && instante - ultimo_log_ms < 1000) return;
    const bool mudou = saude.estado != ultimo_log_saude.estado ||
        saude.causas_ativas != ultimo_log_saude.causas_ativas ||
        saude.erros_sensores != ultimo_log_saude.erros_sensores ||
        saude.erros_i2c != ultimo_log_saude.erros_i2c ||
        saude.erros_uart != ultimo_log_saude.erros_uart ||
        saude.falhas_ota != ultimo_log_saude.falhas_ota ||
        saude.falhas_supervisao != ultimo_log_saude.falhas_supervisao ||
        modulo_mudou ||
        saude.wifi_conectado != ultimo_log_saude.wifi_conectado ||
        saude.equipe_respondendo != ultimo_log_saude.equipe_respondendo ||
        saude.visitantes_respondendo != ultimo_log_saude.visitantes_respondendo ||
        ota.estado != ultimo_log_ota.estado ||
        ota.equipe.estado != ultimo_log_ota.equipe.estado ||
        ota.visitantes.estado != ultimo_log_ota.visitantes.estado ||
        wifi.ultimo_erro != ultimo_log_wifi.ultimo_erro ||
        equipe.confirmacoes_invalidas != ultimo_log_equipe.confirmacoes_invalidas ||
        visitantes.confirmacoes_invalidas != ultimo_log_visitantes.confirmacoes_invalidas ||
        registro.sincronizacoes_finalizadas != ultimo_log_sincronizacoes ||
        registro.amostras_descartadas != ultimo_log_descartes ||
        registro.ultimo_erro != ultimo_log_erro_csv;
    if (ultimo_log_ms != 0 && !mudou && !falha_nova &&
        !ha_historico_pendente &&
        instante - ultimo_log_ms < intervalo) return;

    FILE* arquivo = nullptr;
    bool novo = caminho_log[0] == '\0';
    if (!novo) {
        struct stat info{};
        if (stat(caminho_log, &info) != 0) goto falhou;
        novo = !ha_historico_pendente &&
            static_cast<size_t>(info.st_size) >= configuracao::TAMANHO_SEGMENTO_LOG_ESP;
    }
    if (novo) {
        if (criar_diretorio(configuracao::DIRETORIO_LOG_ESP) != ESP_OK) goto falhou;
        // Exclusivo: um identificador de sessão repetido nunca sobrescreve logs.
        for (unsigned tentativa = 0; tentativa < 32 && arquivo == nullptr; ++tentativa) {
            std::snprintf(caminho_log, sizeof(caminho_log), "%s/esp_%08" PRIu32 ".log",
                          configuracao::DIRETORIO_LOG_ESP, numero_log++);
            arquivo = std::fopen(caminho_log, "wx");
            if (arquivo == nullptr && errno != EEXIST) break;
        }
    } else {
        arquivo = std::fopen(caminho_log, "a+");
    }
    if (arquivo == nullptr) goto falhou;
    // Em modo append, a posição inicial do stream pode ser zero. Posiciona
    // explicitamente no fim para contabilizar só os bytes deste registro.
    if (std::fseek(arquivo, 0, SEEK_END) != 0) goto falhou;
    {
        size_t lote_historico = 0;
        uint64_t ordem_incidente = 0;
        uint32_t incidentes_perdidos = 0;
        if (mutex_historico != nullptr &&
            xSemaphoreTake(mutex_historico, pdMS_TO_TICKS(10)) == pdTRUE) {
            lote_historico = quantidade_pendente;
            ordem_incidente = ordem_incidente_pendente;
            incidentes_perdidos = incidentes_nao_retidos;
            xSemaphoreGive(mutex_historico);
        }
        bool historico_ja_gravado = false;
        if (lote_historico > 0 && !novo && historico_parcial_possivel &&
            ordem_historico_tentativa == ordem_incidente &&
            preparar_historico_no_arquivo(arquivo, sessao,
                                          ordem_incidente, lote_historico,
                                          &historico_ja_gravado) != ESP_OK) {
            goto falhou;
        }
        const long inicio = std::ftell(arquivo);
        if (novo) {
            esp_chip_info_t chip{};
            esp_chip_info(&chip);
            uint32_t flash = 0;
            (void)esp_flash_get_size(nullptr, &flash);
            const auto* app = esp_app_get_description();
            std::fprintf(arquivo, "BOOT sessao=%08" PRIx32 " reset=%d firmware=%s idf=%s cores=%u revisao=%u flash_bytes=%" PRIu32 "\n",
                         sessao, static_cast<int>(esp_reset_reason()), app->version,
                         esp_get_idf_version(), chip.cores, chip.revision, flash);
            std::fprintf(arquivo, "Tempos em ms desde boot; utc=indisponivel ate resposta HTTPS Date; cpu_decimos: 1000=um nucleo; nucleo=afinidade (-1=livre); estado=FreeRTOS eTaskState. Log estruturado, sem credenciais.\n");
        }
        if (lote_historico > 0 && !historico_ja_gravado) {
            ordem_historico_tentativa = ordem_incidente;
            historico_parcial_possivel = true;
            const uint32_t ms_incidente =
                historico_pendente[lote_historico - 1].ms;
            std::fprintf(arquivo, "HIST_INICIO sessao=%08" PRIx32
                         " incidente_ordem=%llu amostras=%u\n", sessao,
                         static_cast<unsigned long long>(ordem_incidente),
                         static_cast<unsigned>(lote_historico));
            for (size_t i = 0; i < lote_historico; ++i) {
                const auto& h = historico_pendente[i];
                std::fprintf(arquivo,
                    "HIST ordem=%llu antes_ms=%" PRIu32 " ms=%" PRIu32
                    " utc_epoch=%lld saude=%u causas=0x%" PRIx32
                    " heap=%" PRIu32 " bloco=%" PRIu32 " psram=%" PRIu32
                    " pilha=%" PRIu32 " tarefa_pilha=%s sensores=%" PRIu32
                    " i2c=%" PRIu32 " uart=%" PRIu32 " ota=%" PRIu32
                    " supervisao=%" PRIu32 " telemetria=%" PRIu32
                    " csv_descartes=%" PRIu32 " wifi_desconexoes=%" PRIu32
                    " wifi_erro=%" PRId32 " equipe=%u/%" PRIu32 "/%u/%" PRIu32
                    " visitantes=%u/%" PRIu32 "/%u/%" PRIu32
                    " tarefas_atrasadas=%u\n",
                    static_cast<unsigned long long>(h.ordem),
                    ms_incidente - h.ms, h.ms,
                    static_cast<long long>(h.utc), static_cast<unsigned>(h.saude),
                    h.causas, h.heap_interno, h.bloco_interno, h.psram,
                    h.pilha_minima, h.tarefa_menor_pilha, h.erros_sensores,
                    h.erros_i2c, h.erros_uart, h.falhas_ota,
                    h.falhas_supervisao, h.falhas_telemetria,
                    h.descartes_csv, h.wifi_desconexoes, h.wifi_erro,
                    static_cast<unsigned>(h.sat_equipe_responde),
                    h.sat_equipe_idade_ms,
                    static_cast<unsigned>(h.sat_equipe_rtt_ms),
                    h.link_equipe_invalidas,
                    static_cast<unsigned>(h.sat_visitantes_responde),
                    h.sat_visitantes_idade_ms,
                    static_cast<unsigned>(h.sat_visitantes_rtt_ms),
                    h.link_visitantes_invalidas,
                    static_cast<unsigned>(h.tarefas_atrasadas));
            }
            std::fprintf(arquivo, "HIST_FIM incidente_ordem=%llu\n",
                         static_cast<unsigned long long>(ordem_incidente));
        }
        const uint32_t descartadas_ram = __atomic_load_n(
            &amostras_ram_descartadas, __ATOMIC_RELAXED);
        if (incidentes_perdidos != 0 || descartadas_ram != 0) {
            std::fprintf(arquivo, "HIST_LIMITES incidentes_sem_lote=%" PRIu32
                         " amostras_ram_descartadas=%" PRIu32 "\n",
                         incidentes_perdidos, descartadas_ram);
        }
        const time_t tempo_utc = std::time(nullptr);
        std::tm data_utc{};
        char texto_utc[32] = "indisponivel";
        if (tempo_utc >= 1704067200 && gmtime_r(&tempo_utc, &data_utc) != nullptr) {
            std::strftime(texto_utc, sizeof(texto_utc), "%Y-%m-%dT%H:%M:%SZ", &data_utc);
        }
        std::fprintf(arquivo, "TEMPO utc=%s ms=%" PRIu32 " arquivo_ordem=%" PRIu32 "\n",
                     texto_utc, instante, numero_log - 1U);
        if (falha_nova) {
            std::fprintf(arquivo,
                "EVENTO falha=1 grave=%d saude=%d sensor=%d link=%d wifi=%d ota=%d supervisao=%d registro=%d janela_ms=%" PRIu32 "\n",
                grave, falha_saude_nova, falha_sensor_nova, falha_link_nova,
                falha_wifi_nova, falha_ota_nova, falha_supervisao_nova,
                falha_registro_nova, grave
                    ? configuracao::JANELA_LOG_FALHA_GRAVE_MS
                    : configuracao::JANELA_LOG_FALHA_MS);
        }
        std::fprintf(arquivo, "ESTADO ms=%" PRIu32 " saude=%s causas=0x%" PRIx32 " interno=%" PRIu32 " bloco=%" PRIu32 " interno_min=%" PRIu32 " bloco_min=%" PRIu32 " psram=%" PRIu32 " psram_bloco=%" PRIu32 " psram_min=%" PRIu32 " psram_bloco_min=%" PRIu32 " heap_min=%" PRIu32 " pilha_min=%" PRIu32 " tarefa_pilha=%s dma=%u dma_bloco=%u\n",
                     instante, servico_diagnostico_nome_estado(saude.estado), saude.causas_ativas,
                     saude.heap_interno_livre, saude.maior_bloco_heap_interno,
                     saude.menor_heap_interno_livre,
                     saude.menor_maior_bloco_heap_interno, saude.psram_livre,
                     saude.maior_bloco_psram, saude.menor_psram_livre,
                     saude.menor_maior_bloco_psram,
                     saude.menor_heap_livre, saude.bytes_pilha_livres,
                     saude.tarefa_menor_pilha,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
        std::fprintf(arquivo, "ERROS sensores=%" PRIu32 " i2c=%" PRIu32 " uart=%" PRIu32 " descartes_uart=%" PRIu32 " ota=%" PRIu32 " supervisao=%" PRIu32 " csv=%s descartes_csv=%" PRIu32 "\n",
                     saude.erros_sensores, saude.erros_i2c, saude.erros_uart, saude.descartes_uart,
                     saude.falhas_ota, saude.falhas_supervisao, esp_err_to_name(registro.ultimo_erro), registro.amostras_descartadas);
        std::fprintf(arquivo, "UPLOAD ativo=%d confirmando=%d concluidos=%" PRIu32 "/%" PRIu32
                     " pendentes=%" PRIu32 " bytes=%" PRIu32 "/%" PRIu32
                     " taxa_b_s=%" PRIu32 " eta_s=%" PRIu32 " ultimo=%s wifi=%d rssi=%d\n",
                     registro.sincronizacao_em_andamento, registro.aguardando_confirmacao_upload,
                     registro.arquivos_upload_concluidos, registro.arquivos_upload_total,
                     registro.arquivos_pendentes, registro.arquivo_atual_bytes_enviados,
                     registro.arquivo_atual_bytes_total, registro.velocidade_upload_bytes_s,
                     registro.tempo_restante_upload_s,
                     esp_err_to_name(registro.ultimo_resultado_sincronizacao),
                     saude.wifi_conectado, saude.wifi_rssi_dbm);
        std::fprintf(arquivo,
            "SAT equipe_responde=%d visitante_responde=%d equipe_latencia_ms=%" PRIu32
            " visitante_latencia_ms=%" PRIu32 " equipe_seq=%u visitante_seq=%u"
            " equipe_ack=%" PRIu32 " visitante_ack=%" PRIu32
            " equipe_invalidas=%" PRIu32 " visitante_invalidas=%" PRIu32 "\n",
            saude.equipe_respondendo, saude.visitantes_respondendo,
            saude.latencia_equipe_ms, saude.latencia_visitantes_ms,
            static_cast<unsigned>(equipe.ultima_sequencia_confirmada),
            static_cast<unsigned>(visitantes.ultima_sequencia_confirmada),
            equipe.confirmacoes_recebidas, visitantes.confirmacoes_recebidas,
            equipe.confirmacoes_invalidas, visitantes.confirmacoes_invalidas);
        std::fprintf(arquivo,
            "OTA mestre=%s ultimo=%s falhas=%" PRIu32 " equipe=%s equipe_erro=%s"
            " visitantes=%s visitantes_erro=%s\n",
            servico_ota_nome_estado(ota.estado), esp_err_to_name(ota.ultimo_erro),
            ota.falhas, servico_ota_nome_estado_satelite(ota.equipe.estado),
            esp_err_to_name(ota.equipe.ultimo_erro),
            servico_ota_nome_estado_satelite(ota.visitantes.estado),
            esp_err_to_name(ota.visitantes.ultimo_erro));
        const bool detalhes = novo || falha_nova || ultimo_log_ms == 0 ||
            instante - ultimo_log_tarefas_ms >= configuracao::INTERVALO_LOG_ESP_MS;
        if (detalhes) {
            const auto retrato = servico_diagnostico_obter_retrato();
            std::fprintf(arquivo,
                "SENSORES adc=%s/%" PRIu32 "/%" PRIu32 " mpu=%s/%" PRIu32 "/%" PRIu32
                " encoder=%s/%" PRIu32 "/%" PRIu32 " ina228=%s/%" PRIu32 "/%" PRIu32
                " odometro_nvs=%s/%" PRIu32 "/%" PRIu32 "\n",
                esp_err_to_name(sensores.adc.ultimo_erro), sensores.adc.total,
                sensores.adc.consecutivos,
                esp_err_to_name(sensores.mpu6050.ultimo_erro), sensores.mpu6050.total,
                sensores.mpu6050.consecutivos,
                esp_err_to_name(sensores.encoder.ultimo_erro), sensores.encoder.total,
                sensores.encoder.consecutivos,
                esp_err_to_name(sensores.ina228.ultimo_erro), sensores.ina228.total,
                sensores.ina228.consecutivos,
                esp_err_to_name(sensores.persistencia_odometro.ultimo_erro),
                sensores.persistencia_odometro.total,
                sensores.persistencia_odometro.consecutivos);
            std::fprintf(arquivo,
                "WIFI ativo=%d conectado=%d sessao=%d ultimo=%s motivo_desconexao=%" PRId32
                " tentativas=%" PRIu32 " desconexoes_inesperadas=%" PRIu32 "\n",
                wifi.radio_ativo, wifi.conectado, wifi.sessao_em_andamento,
                esp_err_to_name(wifi.ultimo_erro), wifi.ultimo_motivo_desconexao,
                wifi.tentativas_conexao, wifi.desconexoes_inesperadas);
            std::fprintf(arquivo,
                "FALHA_CONTEXTO i2c_ultimo=%s i2c_erros=%" PRIu32
                " i2c_enderecos_ausentes=%" PRIu32 " telemetria_falhas=%" PRIu32
                " encoder_esq_erros=%" PRIu32 " encoder_dir_erros=%" PRIu32
                " odometro_validacao=%" PRIu32 " odometro_divergencias=%" PRIu32 "\n",
                esp_err_to_name(retrato.i2c.ultimo_erro), retrato.i2c.erros,
                retrato.i2c.enderecos_ausentes,
                retrato.telemetria.pacotes_com_falha,
                retrato.encoder_esquerdo.erros_leitura,
                retrato.encoder_direito.erros_leitura,
                retrato.odometro.erros_validacao,
                retrato.odometro.divergencias_detectadas);
            std::fprintf(arquivo,
                "SAT_DETALHE equipe_idade_ms=%" PRIu32 " equipe_latencia_max_ms=%" PRIu32
                " visitantes_idade_ms=%" PRIu32 " visitantes_latencia_max_ms=%" PRIu32
                " pacotes_gerados=%" PRIu32
                " pacotes_falhos=%" PRIu32 "\n",
                equipe.idade_ultima_confirmacao_ms, equipe.maior_latencia_ms,
                visitantes.idade_ultima_confirmacao_ms,
                visitantes.maior_latencia_ms,
                telemetria.pacotes_gerados, telemetria.pacotes_com_falha);
        }
        for (unsigned i = 0; i < static_cast<unsigned>(ModuloSupervisionado::Quantidade); ++i) {
            const auto& modulo = modulos[i];
            if (!modulo.registrado) continue;
            if (!detalhes && !modulo.atrasado && !modulo.em_recuperacao &&
                modulo.ultimo_resultado == ESP_OK) continue;
            std::fprintf(arquivo, "SUP %s fase=%s idade_ms=%" PRIu32 " limite_ms=%" PRIu32 " atrasado=%d recuperando=%d falhas=%" PRIu32 " ultimo=%s\n",
                         modulo.nome, modulo.fase, modulo.idade_ultimo_pulso_ms,
                         modulo.tempo_limite_ms, modulo.atrasado, modulo.em_recuperacao,
                         modulo.falhas_detectadas, esp_err_to_name(modulo.ultimo_resultado));
        }
        bool tarefas_coletadas = false;
        if (falha_nova && tarefas_log != nullptr) {
            servico_diagnostico_obter_tarefas(tarefas_log);
            tarefas_coletadas = true;
            for (size_t i = 0; i < tarefas_log->quantidade; ++i) {
                const auto& t = tarefas_log->tarefas[i];
                if ((t.pilha_reservada_bytes == 0 ||
                     t.pilha_minima_livre_bytes >= 512) && t.estado < 4) continue;
                std::fprintf(arquivo,
                    "T_ALERTA %s id=%" PRIu32 " estado=%u pilha_livre_min=%" PRIu32
                    " cpu_decimos=%u\n", t.nome, t.identificador, t.estado,
                    t.pilha_minima_livre_bytes, t.cpu_decimos);
            }
        }
        if (novo || ultimo_log_tarefas_ms == 0 ||
            instante - ultimo_log_tarefas_ms >= configuracao::INTERVALO_LOG_ESP_MS ||
            (falha_nova && instante - ultimo_log_tarefas_ms >= 15000)) {
            if (tarefas_log != nullptr) {
                if (!tarefas_coletadas) servico_diagnostico_obter_tarefas(tarefas_log);
                std::fprintf(arquivo, "TAREFAS valido=%d coleta_ms=%llu quantidade=%u\n", tarefas_log->valido,
                             static_cast<unsigned long long>(tarefas_log->coletado_em_ms), static_cast<unsigned>(tarefas_log->quantidade));
                for (size_t i = 0; i < tarefas_log->quantidade; ++i) {
                    const auto& t = tarefas_log->tarefas[i];
                    std::fprintf(arquivo, "T %s id=%" PRIu32 " nucleo=%d prioridade=%" PRIu32 " estado=%u pilha=%" PRIu32 " livre_min=%" PRIu32 " cpu_decimos=%u cpu_valida=%d\n",
                                 t.nome, t.identificador, t.nucleo, t.prioridade, t.estado,
                                 t.pilha_reservada_bytes, t.pilha_minima_livre_bytes, t.cpu_decimos, t.cpu_valida);
                }
            } else {
                std::fprintf(arquivo, "TAREFAS indisponivel: sem PSRAM para retrato\n");
            }
            ultimo_log_tarefas_ms = instante;
        }
        const long fim = std::ftell(arquivo);
        const bool erro = std::ferror(arquivo) || std::fflush(arquivo) != 0 || fsync(fileno(arquivo)) != 0;
        const bool erro_fechamento = std::fclose(arquivo) != 0;
        arquivo = nullptr;
        if (inicio >= 0 && fim >= inicio) bytes_log_sessao += static_cast<size_t>(fim - inicio);
        if (erro || erro_fechamento || inicio < 0 || fim < inicio) goto falhou;
        if (lote_historico > 0 || incidentes_perdidos > 0) {
            if (mutex_historico != nullptr &&
                xSemaphoreTake(mutex_historico, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (lote_historico > 0 &&
                    ordem_incidente_pendente == ordem_incidente) {
                    quantidade_pendente = 0;
                    ordem_incidente_pendente = 0;
                    historico_parcial_possivel = false;
                }
                incidentes_nao_retidos =
                    incidentes_nao_retidos >= incidentes_perdidos
                        ? incidentes_nao_retidos - incidentes_perdidos : 0;
                xSemaphoreGive(mutex_historico);
            }
        }
        if (descartadas_ram != 0) {
            (void)__atomic_fetch_sub(&amostras_ram_descartadas,
                                     descartadas_ram, __ATOMIC_RELAXED);
        }
    }
    ultimo_log_ms = instante;
    ultimo_log_saude = saude;
    ultimos_log_sensores = sensores;
    ultimo_log_telemetria = telemetria;
    ultimo_log_equipe = equipe;
    ultimo_log_visitantes = visitantes;
    ultimo_log_wifi = wifi;
    ultimo_log_ota = ota;
    for (unsigned i = 0; i < static_cast<unsigned>(ModuloSupervisionado::Quantidade); ++i) {
        ultimos_log_modulos[i] = modulos[i];
    }
    ultimo_log_sincronizacoes = registro.sincronizacoes_finalizadas;
    ultimo_log_descartes = registro.amostras_descartadas;
    ultimo_log_erro_csv = registro.ultimo_erro;
    repetir_log_ms = 0;
    return;
falhou:
    if (arquivo != nullptr) std::fclose(arquivo);
    {
        struct stat info{};
        if (stat(caminho_log, &info) != 0) caminho_log[0] = '\0';
    }
    repetir_log_ms = instante + configuracao::INTERVALO_LOG_ESP_MS;
    ESP_LOGW(ETIQUETA, "Falha ao persistir diagnóstico ESP no SD; tentativa em 30 s (errno=%d)", errno);
}

uint32_t agora_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void tarefa_historico(void*) {
    AmostraHistorico anterior{};
    bool tem_anterior = false;
    uint32_t proxima_captura_ms = 0;
    bool ultima_captura_grave = false;
    while (true) {
        const auto saude = servico_diagnostico_obter_resumo();
        if (saude.valido) {
            const auto equipe = servico_telemetria_obter_estatisticas_link(
                SateliteTelemetria::Equipe);
            const auto visitantes = servico_telemetria_obter_estatisticas_link(
                SateliteTelemetria::Visitantes);
            const auto wifi = servico_wifi_obter_resumo();
            const auto registro = servico_registro_telemetria_obter_estado();
            const auto telemetria = servico_telemetria_obter_estatisticas();
            AmostraHistorico amostra{};
            amostra.ms = agora_ms();
            const time_t utc = std::time(nullptr);
            amostra.utc = utc >= 1704067200 ? static_cast<int64_t>(utc) : 0;
            amostra.causas = saude.causas_ativas;
            amostra.heap_interno = saude.heap_interno_livre;
            amostra.bloco_interno = saude.maior_bloco_heap_interno;
            amostra.psram = saude.psram_livre;
            amostra.pilha_minima = saude.bytes_pilha_livres;
            amostra.erros_sensores = saude.erros_sensores;
            amostra.erros_i2c = saude.erros_i2c;
            amostra.erros_uart = saude.erros_uart;
            amostra.falhas_ota = saude.falhas_ota;
            amostra.falhas_supervisao = saude.falhas_supervisao;
            amostra.falhas_telemetria = telemetria.pacotes_com_falha;
            amostra.descartes_csv = registro.amostras_descartadas;
            amostra.wifi_desconexoes = wifi.desconexoes_inesperadas;
            amostra.wifi_erro = static_cast<int32_t>(wifi.ultimo_erro);
            amostra.link_equipe_invalidas = equipe.confirmacoes_invalidas;
            amostra.link_visitantes_invalidas = visitantes.confirmacoes_invalidas;
            amostra.sat_equipe_idade_ms = equipe.idade_ultima_confirmacao_ms;
            amostra.sat_visitantes_idade_ms = visitantes.idade_ultima_confirmacao_ms;
            amostra.sat_equipe_rtt_ms = static_cast<uint16_t>(
                std::min<uint32_t>(equipe.latencia_atual_ms, UINT16_MAX));
            amostra.sat_visitantes_rtt_ms = static_cast<uint16_t>(
                std::min<uint32_t>(visitantes.latencia_atual_ms, UINT16_MAX));
            amostra.saude = static_cast<uint8_t>(saude.estado);
            amostra.sat_equipe_responde = saude.equipe_respondendo;
            amostra.sat_visitantes_responde = saude.visitantes_respondendo;
            std::strncpy(amostra.tarefa_menor_pilha, saude.tarefa_menor_pilha,
                         sizeof(amostra.tarefa_menor_pilha) - 1);
            for (unsigned i = 0; i < static_cast<unsigned>(ModuloSupervisionado::Quantidade); ++i) {
                const auto modulo = servico_supervisao_obter_estado(
                    static_cast<ModuloSupervisionado>(i));
                if ((modulo.atrasado || modulo.em_recuperacao) &&
                    amostra.tarefas_atrasadas < UINT8_MAX) {
                    amostra.tarefas_atrasadas++;
                }
            }
            const bool falha =
                (!tem_anterior && (amostra.causas != 0 ||
                    amostra.saude == static_cast<uint8_t>(EstadoSaudeSistema::Falha))) ||
                (tem_anterior && (
                    (amostra.causas & ~anterior.causas) != 0 ||
                    amostra.erros_sensores > anterior.erros_sensores ||
                    amostra.erros_i2c > anterior.erros_i2c ||
                    amostra.erros_uart > anterior.erros_uart ||
                    amostra.falhas_ota > anterior.falhas_ota ||
                    amostra.falhas_supervisao > anterior.falhas_supervisao ||
                    amostra.falhas_telemetria > anterior.falhas_telemetria ||
                    amostra.descartes_csv > anterior.descartes_csv ||
                    amostra.wifi_desconexoes > anterior.wifi_desconexoes ||
                    (amostra.wifi_erro != ESP_OK &&
                     amostra.wifi_erro != anterior.wifi_erro) ||
                    amostra.link_equipe_invalidas > anterior.link_equipe_invalidas ||
                    amostra.link_visitantes_invalidas > anterior.link_visitantes_invalidas ||
                    (anterior.sat_equipe_responde && !amostra.sat_equipe_responde) ||
                    (anterior.sat_visitantes_responde && !amostra.sat_visitantes_responde) ||
                    amostra.tarefas_atrasadas > anterior.tarefas_atrasadas ||
                    (amostra.saude == static_cast<uint8_t>(EstadoSaudeSistema::Falha) &&
                     anterior.saude != amostra.saude)));
            const bool grave =
                amostra.saude == static_cast<uint8_t>(EstadoSaudeSistema::Falha) ||
                (amostra.causas & (CausaSaudeHeapCritico |
                                   CausaSaudePilhaCritica |
                                   CausaSaudeSupervisao)) != 0 ||
                amostra.tarefas_atrasadas != 0;
            const bool capturar_incidente = falha &&
                (proxima_captura_ms == 0 ||
                 static_cast<int32_t>(amostra.ms - proxima_captura_ms) >= 0 ||
                 (grave && !ultima_captura_grave));
            bool registrada = false;
            if (xSemaphoreTake(mutex_historico, pdMS_TO_TICKS(100)) == pdTRUE) {
                amostra.ordem = ++ordem_amostra;
                if (capturar_incidente) {
                    if (quantidade_pendente == 0) {
                        for (size_t i = 0; i < quantidade_historico; ++i) {
                            historico_pendente[i] = amostras_historico[
                                (inicio_historico + i) % capacidade_historico];
                        }
                        historico_pendente[quantidade_historico] = amostra;
                        quantidade_pendente = quantidade_historico + 1;
                        ordem_incidente_pendente = amostra.ordem;
                    } else if (incidentes_nao_retidos < UINT32_MAX) {
                        incidentes_nao_retidos++;
                    }
                    proxima_captura_ms = amostra.ms + (grave
                        ? configuracao::JANELA_LOG_FALHA_GRAVE_MS
                        : configuracao::JANELA_LOG_FALHA_MS);
                    ultima_captura_grave = grave;
                }
                if (quantidade_historico == capacidade_historico) {
                    amostras_historico[inicio_historico] = amostra;
                    inicio_historico = (inicio_historico + 1) % capacidade_historico;
                } else {
                    amostras_historico[(inicio_historico + quantidade_historico) %
                                       capacidade_historico] = amostra;
                    quantidade_historico++;
                }
                registrada = true;
                xSemaphoreGive(mutex_historico);
            } else {
                (void)__atomic_fetch_add(&amostras_ram_descartadas, 1U,
                                         __ATOMIC_RELAXED);
            }
            if (registrada) {
                anterior = amostra;
                tem_anterior = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void iniciar_historico_ram() {
    if (tarefa_historico_handle != nullptr) return;
    if (mutex_historico == nullptr) mutex_historico = xSemaphoreCreateMutex();
    if (mutex_historico == nullptr) return;
    if (amostras_historico == nullptr) {
        capacidade_historico = AMOSTRAS_HISTORICO_PSRAM;
        amostras_historico = static_cast<AmostraHistorico*>(heap_caps_calloc(
            2 * capacidade_historico + 1, sizeof(AmostraHistorico),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (amostras_historico == nullptr) {
            capacidade_historico = AMOSTRAS_HISTORICO_INTERNO;
            amostras_historico = static_cast<AmostraHistorico*>(heap_caps_calloc(
                2 * capacidade_historico + 1, sizeof(AmostraHistorico),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (amostras_historico == nullptr) {
            capacidade_historico = 0;
            ESP_LOGW(ETIQUETA, "Sem memória para histórico pré-falha; diagnóstico SD continua ativo");
            return;
        }
        historico_pendente = amostras_historico + capacidade_historico;
    }
    if (configuracao::criar_tarefa(tarefa_historico, "historico_sd", nullptr,
                                   &tarefa_historico_handle) != pdPASS) {
        tarefa_historico_handle = nullptr;
        heap_caps_free(amostras_historico);
        amostras_historico = nullptr;
        historico_pendente = nullptr;
        capacidade_historico = 0;
        ESP_LOGW(ETIQUETA, "Falha ao iniciar amostragem pré-falha");
    }
}

bool termina_com(const char* texto, const char* sufixo) {
    if (texto == nullptr || sufixo == nullptr) return false;
    const size_t tamanho_texto = std::strlen(texto);
    const size_t tamanho_sufixo = std::strlen(sufixo);
    return tamanho_texto >= tamanho_sufixo &&
           std::strcmp(texto + tamanho_texto - tamanho_sufixo, sufixo) == 0;
}

void definir_ultimo_erro(esp_err_t erro) {
    portENTER_CRITICAL(&trava_estado);
    estado.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_estado);
}

void incrementar_descartadas() {
    portENTER_CRITICAL(&trava_estado);
    estado.amostras_descartadas++;
    estado.ultimo_erro = ESP_ERR_NO_MEM;
    portEXIT_CRITICAL(&trava_estado);
}

esp_err_t criar_diretorio(const char* caminho) {
    if (mkdir(caminho, 0775) == 0 || errno == EEXIST) return ESP_OK;
    ESP_LOGE(ETIQUETA, "Falha ao criar diretório %s: errno=%d", caminho,
             errno);
    return ESP_FAIL;
}

uint32_t contar_arquivos_pendentes() {
    uint32_t quantidade = 0;
    DIR* diretorio = opendir(configuracao::DIRETORIO_TELEMETRIA);
    if (diretorio == nullptr) return 0;
    while (dirent* entrada = readdir(diretorio)) {
        if (entrada->d_name[0] != '.' && termina_com(entrada->d_name, ".csv")) {
            quantidade++;
        }
    }
    closedir(diretorio);
    return quantidade;
}

void atualizar_pendentes() {
    const uint32_t quantidade = contar_arquivos_pendentes();
    portENTER_CRITICAL(&trava_estado);
    estado.arquivos_pendentes = quantidade;
    portEXIT_CRITICAL(&trava_estado);
}

esp_err_t recuperar_arquivos_interrompidos() {
    DIR* diretorio = opendir(configuracao::DIRETORIO_TELEMETRIA);
    if (diretorio == nullptr) return ESP_FAIL;
    esp_err_t resultado = ESP_OK;
    while (dirent* entrada = readdir(diretorio)) {
        if (!termina_com(entrada->d_name, ".tmp")) continue;
        char origem[192]{};
        char destino[192]{};
        const int escritos_origem = std::snprintf(
            origem, sizeof(origem), "%s/%s",
            configuracao::DIRETORIO_TELEMETRIA, entrada->d_name);
        char nome[96]{};
        std::strncpy(nome, entrada->d_name, sizeof(nome) - 1);
        char* extensao = std::strrchr(nome, '.');
        if (extensao != nullptr) std::strcpy(extensao, ".csv");
        const int escritos_destino = std::snprintf(
            destino, sizeof(destino), "%s/%s",
            configuracao::DIRETORIO_TELEMETRIA, nome);
        bool recuperavel = false;
        FILE* arquivo = escritos_origem > 0 &&
                                escritos_origem < static_cast<int>(sizeof(origem))
                            ? std::fopen(origem, "rb")
                            : nullptr;
        if (arquivo != nullptr) {
            char cabecalho[sizeof(CABECALHO_CSV)]{};
            const size_t lidos = std::fread(
                cabecalho, 1, sizeof(CABECALHO_CSV) - 1, arquivo);
            if (lidos == sizeof(CABECALHO_CSV) - 1 &&
                std::memcmp(cabecalho, CABECALHO_CSV, lidos) == 0 &&
                std::fseek(arquivo, -1, SEEK_END) == 0) {
                recuperavel = std::fgetc(arquivo) == '\n';
            }
            std::fclose(arquivo);
        }
        if (!recuperavel && escritos_origem > 0 &&
            escritos_origem < static_cast<int>(sizeof(origem))) {
            char nome_incompleto[96]{};
            std::strncpy(nome_incompleto, entrada->d_name,
                         sizeof(nome_incompleto) - 1);
            char* extensao_incompleta = std::strrchr(nome_incompleto, '.');
            const size_t prefixo = extensao_incompleta == nullptr
                                       ? sizeof(nome_incompleto)
                                       : static_cast<size_t>(extensao_incompleta -
                                                             nome_incompleto);
            if (extensao_incompleta != nullptr &&
                prefixo + sizeof(".incompleto") <= sizeof(nome_incompleto)) {
                std::strcpy(extensao_incompleta, ".incompleto");
            } else {
                extensao_incompleta = nullptr;
            }
            char caminho_incompleto[192]{};
            const int escritos_incompleto = std::snprintf(
                caminho_incompleto, sizeof(caminho_incompleto), "%s/%s",
                configuracao::DIRETORIO_TELEMETRIA, nome_incompleto);
            if (extensao_incompleta != nullptr && escritos_incompleto > 0 &&
                escritos_incompleto < static_cast<int>(sizeof(caminho_incompleto)) &&
                rename(origem, caminho_incompleto) == 0) {
                ESP_LOGW(ETIQUETA,
                         "CSV interrompido preservado sem upload: %s",
                         nome_incompleto);
                continue;
            }
        }
        if (!recuperavel || escritos_origem <= 0 ||
            escritos_origem >= static_cast<int>(sizeof(origem)) ||
            escritos_destino <= 0 ||
            escritos_destino >= static_cast<int>(sizeof(destino)) ||
            rename(origem, destino) != 0) {
            resultado = ESP_FAIL;
            if (recuperavel) {
                ESP_LOGE(ETIQUETA,
                         "Não foi possível recuperar o arquivo temporário %s",
                         entrada->d_name);
            } else {
                ESP_LOGE(ETIQUETA,
                         "Arquivo temporário incompleto preservado: %s",
                         entrada->d_name);
            }
        } else {
            ESP_LOGW(ETIQUETA, "CSV interrompido recuperado: %s", nome);
        }
    }
    closedir(diretorio);
    return resultado;
}

esp_err_t abrir_novo_arquivo(uint32_t instante_ms) {
    if (arquivo_atual != nullptr) return ESP_ERR_INVALID_STATE;
    const unsigned long id_sessao = static_cast<unsigned long>(sessao);
    const unsigned long id_arquivo =
        static_cast<unsigned long>(sequencia_arquivo++);
    char nome_temporario[96]{};
    const int escritos_nome = std::snprintf(
        nome_temporario, sizeof(nome_temporario),
        "telemetria_%08lx_%04lu.tmp", id_sessao, id_arquivo);
    const int escritos_final = std::snprintf(
        nome_final_atual, sizeof(nome_final_atual),
        "telemetria_%08lx_%04lu.csv", id_sessao, id_arquivo);
    const int escritos_caminho = std::snprintf(
        caminho_temporario, sizeof(caminho_temporario), "%s/%s",
        configuracao::DIRETORIO_TELEMETRIA, nome_temporario);
    if (escritos_nome <= 0 ||
        escritos_nome >= static_cast<int>(sizeof(nome_temporario)) ||
        escritos_final <= 0 ||
        escritos_final >= static_cast<int>(sizeof(nome_final_atual)) ||
        escritos_caminho <= 0 ||
        escritos_caminho >= static_cast<int>(sizeof(caminho_temporario))) {
        return ESP_ERR_INVALID_SIZE;
    }

    arquivo_atual = std::fopen(caminho_temporario, "wb");
    if (arquivo_atual == nullptr) return ESP_FAIL;
    if (std::fwrite(CABECALHO_CSV, 1, sizeof(CABECALHO_CSV) - 1,
                    arquivo_atual) != sizeof(CABECALHO_CSV) - 1) {
        std::fclose(arquivo_atual);
        arquivo_atual = nullptr;
        return ESP_FAIL;
    }
    inicio_arquivo_ms = instante_ms;
    linhas_sem_sincronizar = 0;
    ESP_LOGI(ETIQUETA, "Registro iniciado em %s", nome_temporario);
    return ESP_OK;
}

esp_err_t sincronizar_arquivo_aberto() {
    if (arquivo_atual == nullptr) return ESP_OK;
    if (std::fflush(arquivo_atual) != 0 || fsync(fileno(arquivo_atual)) != 0) {
        return ESP_FAIL;
    }
    linhas_sem_sincronizar = 0;
    return ESP_OK;
}

esp_err_t fechar_arquivo_atual() {
    if (arquivo_atual == nullptr) return ESP_OK;
    esp_err_t resultado = sincronizar_arquivo_aberto();
    if (std::fclose(arquivo_atual) != 0 && resultado == ESP_OK) {
        resultado = ESP_FAIL;
    }
    arquivo_atual = nullptr;

    if (resultado == ESP_OK) {
        char caminho_final[192]{};
        const int escritos = std::snprintf(
            caminho_final, sizeof(caminho_final), "%s/%s",
            configuracao::DIRETORIO_TELEMETRIA, nome_final_atual);
        if (escritos <= 0 ||
            escritos >= static_cast<int>(sizeof(caminho_final)) ||
            rename(caminho_temporario, caminho_final) != 0) {
            ESP_LOGE(ETIQUETA, "Falha ao concluir CSV %s", caminho_temporario);
            resultado = ESP_FAIL;
        } else {
            ESP_LOGI(ETIQUETA, "CSV pronto para upload: %s", nome_final_atual);
        }
    } else {
        ESP_LOGE(ETIQUETA,
                 "CSV não foi publicado porque a sincronização com o SD falhou: %s",
                 caminho_temporario);
    }
    caminho_temporario[0] = '\0';
    nome_final_atual[0] = '\0';
    atualizar_pendentes();
    return resultado;
}

esp_err_t formatar_linha_csv(const ComandoRegistro& comando, char* linha,
                             size_t capacidade, size_t* tamanho) {
    const auto& d = comando.dados;
    const int escritos = std::snprintf(
        linha, capacidade,
        "%" PRIu32 ",%u,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.3f,%u,%u,%u,%u,%d,%u,%.3f,%.6f,%.6f,%.2f,%.2f,"
        "%.3f,%.3f,%.2f,%.4f,%.4f,%.4f,%.4f,%.7f,%.7f,%.2f,%.2f,%.2f,"
        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.6f,%.6f,%u\n",
        comando.tempo_sessao_ms, static_cast<unsigned>(d.tensao_adc_bruta),
        d.aceleracao_x, d.aceleracao_y, d.aceleracao_z, d.giroscopio_x,
        d.giroscopio_y, d.giroscopio_z, d.aceleracao_x_g, d.aceleracao_y_g,
        d.aceleracao_z_g, d.aceleracao_x_ms2, d.aceleracao_y_ms2,
        d.aceleracao_z_ms2, d.giroscopio_x_graus_s,
        d.giroscopio_y_graus_s, d.giroscopio_z_graus_s, d.temperatura_mpu_c,
        d.mpu_ativo, d.mpu_calibrado, d.eixo_longitudinal_compativel,
        static_cast<unsigned>(d.eixo_longitudinal_mpu),
        static_cast<int>(d.sentido_aceleracao_longitudinal),
        static_cast<unsigned>(d.marcha), d.velocidade_kmh, d.aceleracao_ms2,
        d.aceleracao_g, d.acelerador_percentual, d.freio_percentual,
        d.tensao_pacote_v, d.corrente_a, d.carga_percentual,
        d.tensoes_celulas_v[0], d.tensoes_celulas_v[1], d.tensoes_celulas_v[2],
        d.tensoes_celulas_v[3], d.latitude, d.longitude, d.rumo_graus,
        d.distancias_cm[0], d.distancias_cm[1], d.distancias_cm[2],
        d.distancias_cm[3], d.distancias_cm[4], d.distancias_cm[5],
        d.distancias_cm[6], d.distancias_cm[7], d.potencia_w, d.autonomia_km,
        d.percurso_km, d.odometro_km, d.simulado);
    if (escritos <= 0 || escritos >= static_cast<int>(capacidade)) {
        return ESP_ERR_INVALID_SIZE;
    }
    *tamanho = static_cast<size_t>(escritos);
    return ESP_OK;
}

esp_err_t gravar_amostra(const ComandoRegistro& comando) {
    if (arquivo_atual == nullptr) {
        const esp_err_t erro = abrir_novo_arquivo(comando.tempo_sessao_ms);
        if (erro != ESP_OK) return erro;
    }

    char linha[1024]{};
    size_t tamanho = 0;
    esp_err_t erro = formatar_linha_csv(comando, linha, sizeof(linha), &tamanho);
    if (erro != ESP_OK) return erro;
    if (std::fwrite(linha, 1, tamanho, arquivo_atual) != tamanho) {
        ESP_LOGE(ETIQUETA,
                 "Falha ao gravar no SD; o arquivo temporário será preservado");
        (void)std::fclose(arquivo_atual);
        arquivo_atual = nullptr;
        caminho_temporario[0] = '\0';
        nome_final_atual[0] = '\0';
        linhas_sem_sincronizar = 0;
        return ESP_FAIL;
    }

    linhas_sem_sincronizar++;
    portENTER_CRITICAL(&trava_estado);
    estado.amostras_gravadas++;
    estado.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava_estado);

    if (linhas_sem_sincronizar *
            configuracao::INTERVALO_REGISTRO_TELEMETRIA_MS >=
        configuracao::INTERVALO_SINCRONIZACAO_SD_MS) {
        erro = sincronizar_arquivo_aberto();
        if (erro != ESP_OK) return erro;
    }
    const long posicao = std::ftell(arquivo_atual);
    if (comando.tempo_sessao_ms - inicio_arquivo_ms >=
            configuracao::DURACAO_ARQUIVO_TELEMETRIA_MS ||
        (posicao >= 0 &&
         static_cast<size_t>(posicao) >=
             configuracao::TAMANHO_MAXIMO_CSV_BYTES)) {
        return fechar_arquivo_atual();
    }
    return ESP_OK;
}

esp_err_t tratar_evento_http(esp_http_client_event_t* evento) {
    if (evento == nullptr || evento->user_data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* resposta = static_cast<RespostaHttp*>(evento->user_data);
    if (evento->event_id == HTTP_EVENT_ON_HEADER &&
        evento->header_key != nullptr && evento->header_value != nullptr &&
        strcasecmp(evento->header_key, "Date") == 0) {
        acertar_relogio_http_interno(evento->header_value);
    }
    if (evento->event_id == HTTP_EVENT_ON_HEADER &&
        evento->header_key != nullptr && evento->header_value != nullptr &&
        strcasecmp(evento->header_key, "Location") == 0) {
        const size_t tamanho = std::strlen(evento->header_value);
        if (tamanho < sizeof(resposta->redirecionamento)) {
            std::memcpy(resposta->redirecionamento, evento->header_value,
                        tamanho + 1);
        }
        return ESP_OK;
    }
    if (evento->event_id != HTTP_EVENT_ON_DATA || evento->data == nullptr ||
        evento->data_len <= 0) {
        return ESP_OK;
    }
    const size_t disponivel = sizeof(resposta->texto) - 1 - resposta->tamanho;
    const size_t copiar = std::min(
        disponivel, static_cast<size_t>(evento->data_len));
    if (copiar > 0) {
        std::memcpy(resposta->texto + resposta->tamanho, evento->data, copiar);
        resposta->tamanho += copiar;
        resposta->texto[resposta->tamanho] = '\0';
    }
    return ESP_OK;
}

bool caractere_url_seguro(unsigned char caractere) {
    return (caractere >= 'a' && caractere <= 'z') ||
           (caractere >= 'A' && caractere <= 'Z') ||
           (caractere >= '0' && caractere <= '9') || caractere == '-' ||
           caractere == '_' || caractere == '.' || caractere == '~';
}

esp_err_t acrescentar_parametro_url(char* destino, size_t capacidade,
                                    size_t* usados, const char* valor) {
    constexpr char HEXADECIMAL[] = "0123456789ABCDEF";
    for (const unsigned char* atual =
             reinterpret_cast<const unsigned char*>(valor);
         *atual != '\0'; ++atual) {
        if (caractere_url_seguro(*atual)) {
            if (*usados + 1 >= capacidade) return ESP_ERR_INVALID_SIZE;
            destino[(*usados)++] = static_cast<char>(*atual);
        } else {
            if (*usados + 3 >= capacidade) return ESP_ERR_INVALID_SIZE;
            destino[(*usados)++] = '%';
            destino[(*usados)++] = HEXADECIMAL[*atual >> 4];
            destino[(*usados)++] = HEXADECIMAL[*atual & 0x0F];
        }
    }
    destino[*usados] = '\0';
    return ESP_OK;
}

esp_err_t montar_url_upload(const char* nome, char* url, size_t capacidade) {
    const char* base = configuracao::ENDERECO_GOOGLE_TELEMETRIA;
    const size_t tamanho_base = std::strlen(base);
    if (tamanho_base == 0 || tamanho_base + 18 >= capacidade) {
        return ESP_ERR_INVALID_STATE;
    }
    std::memcpy(url, base, tamanho_base);
    size_t usados = tamanho_base;
    url[usados++] = std::strchr(base, '?') == nullptr ? '?' : '&';
    constexpr char PREFIXO_TOKEN[] = "token=";
    std::memcpy(url + usados, PREFIXO_TOKEN, sizeof(PREFIXO_TOKEN) - 1);
    usados += sizeof(PREFIXO_TOKEN) - 1;
    esp_err_t erro = acrescentar_parametro_url(
        url, capacidade, &usados, configuracao::TOKEN_GOOGLE_TELEMETRIA);
    if (erro != ESP_OK) return erro;
    constexpr char PREFIXO_ARQUIVO[] = "&arquivo=";
    if (usados + sizeof(PREFIXO_ARQUIVO) >= capacidade) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(url + usados, PREFIXO_ARQUIVO,
                sizeof(PREFIXO_ARQUIVO) - 1);
    usados += sizeof(PREFIXO_ARQUIVO) - 1;
    return acrescentar_parametro_url(url, capacidade, &usados, nome);
}

esp_err_t abrir_arquivo_upload(const char* caminho, FILE** arquivo,
                               size_t* tamanho) {
    if (arquivo == nullptr || tamanho == nullptr) return ESP_ERR_INVALID_ARG;
    *arquivo = std::fopen(caminho, "rb");
    if (*arquivo == nullptr) return ESP_ERR_NOT_FOUND;
    if (std::fseek(*arquivo, 0, SEEK_END) != 0) {
        std::fclose(*arquivo);
        *arquivo = nullptr;
        return ESP_FAIL;
    }
    const long bytes = std::ftell(*arquivo);
    if (bytes <= 0 ||
        static_cast<size_t>(bytes) >
            configuracao::TAMANHO_MAXIMO_CSV_BYTES ||
        std::fseek(*arquivo, 0, SEEK_SET) != 0) {
        std::fclose(*arquivo);
        *arquivo = nullptr;
        return ESP_ERR_INVALID_SIZE;
    }
    *tamanho = static_cast<size_t>(bytes);
    return ESP_OK;
}

void iniciar_progresso_arquivo(const char* nome, size_t tamanho) {
    portENTER_CRITICAL(&trava_estado);
    std::strncpy(estado.arquivo_em_upload, nome,
                 sizeof(estado.arquivo_em_upload) - 1);
    estado.arquivo_em_upload[sizeof(estado.arquivo_em_upload) - 1] = '\0';
    estado.arquivo_atual_bytes_enviados = 0;
    estado.arquivo_atual_bytes_total = static_cast<uint32_t>(tamanho);
    estado.velocidade_upload_bytes_s = 0;
    estado.tempo_restante_upload_s = 0;
    estado.aguardando_confirmacao_upload = false;
    portEXIT_CRITICAL(&trava_estado);
}

void atualizar_progresso_arquivo(size_t enviados, size_t tamanho,
                                 uint32_t inicio_ms) {
    const uint32_t decorrido_ms = std::max<uint32_t>(1, agora_ms() - inicio_ms);
    const uint32_t velocidade = static_cast<uint32_t>(
        (static_cast<uint64_t>(enviados) * 1000U) / decorrido_ms);
    const uint32_t restantes = enviados >= tamanho
                                   ? 0
                                   : static_cast<uint32_t>(tamanho - enviados);
    const uint32_t tempo_restante = velocidade == 0
        ? 0
        : static_cast<uint32_t>(
              (static_cast<uint64_t>(restantes) + velocidade - 1U) /
              velocidade);
    portENTER_CRITICAL(&trava_estado);
    estado.arquivo_atual_bytes_enviados = static_cast<uint32_t>(enviados);
    estado.velocidade_upload_bytes_s = velocidade;
    estado.tempo_restante_upload_s = tempo_restante;
    portEXIT_CRITICAL(&trava_estado);
}

void definir_aguardando_confirmacao_upload(bool aguardando) {
    portENTER_CRITICAL(&trava_estado);
    estado.aguardando_confirmacao_upload = aguardando;
    portEXIT_CRITICAL(&trava_estado);
}

esp_err_t transmitir_arquivo(esp_http_client_handle_t cliente, FILE* arquivo,
                             size_t tamanho) {
    esp_err_t erro = esp_http_client_open(cliente, static_cast<int>(tamanho));
    if (erro != ESP_OK) return erro;

    char* bloco = static_cast<char*>(heap_caps_malloc(
        configuracao::TAMANHO_BLOCO_UPLOAD_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bloco == nullptr) {
        bloco = static_cast<char*>(heap_caps_malloc(
            configuracao::TAMANHO_BLOCO_UPLOAD_BYTES, MALLOC_CAP_8BIT));
    }
    if (bloco == nullptr) return ESP_ERR_NO_MEM;

    const uint32_t inicio_ms = agora_ms();
    uint32_t proximo_relatorio_ms =
        inicio_ms + configuracao::INTERVALO_RELATORIO_UPLOAD_MS;
    size_t enviados = 0;
    while (enviados < tamanho) {
        servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
            "upload_csv", "leitura SD/envio HTTPS", 90000);
        const size_t desejados = std::min(
            configuracao::TAMANHO_BLOCO_UPLOAD_BYTES, tamanho - enviados);
        const size_t lidos = std::fread(bloco, 1, desejados, arquivo);
        if (lidos != desejados) {
            erro = ESP_FAIL;
            break;
        }
        size_t escritos_bloco = 0;
        while (escritos_bloco < lidos) {
            const int escritos = esp_http_client_write(
                cliente, bloco + escritos_bloco,
                static_cast<int>(lidos - escritos_bloco));
            if (escritos <= 0) {
                erro = ESP_ERR_HTTP_WRITE_DATA;
                break;
            }
            escritos_bloco += static_cast<size_t>(escritos);
            enviados += static_cast<size_t>(escritos);
        }
        if (erro != ESP_OK) break;
        atualizar_progresso_arquivo(enviados, tamanho, inicio_ms);
        const uint32_t instante_ms = agora_ms();
        if (enviados == tamanho ||
            static_cast<int32_t>(instante_ms - proximo_relatorio_ms) >= 0) {
            const EstadoRegistroTelemetria progresso =
                servico_registro_telemetria_obter_estado();
            const uint32_t percentual_decimos = tamanho == 0
                ? 0
                : static_cast<uint32_t>(
                      (static_cast<uint64_t>(enviados) * 1000U) / tamanho);
            ESP_LOGI(
                ETIQUETA,
                "Upload %lu.%lu%% (%lu/%lu KiB), %lu.%lu KiB/s, restante %lu s",
                static_cast<unsigned long>(percentual_decimos / 10U),
                static_cast<unsigned long>(percentual_decimos % 10U),
                static_cast<unsigned long>(enviados / 1024U),
                static_cast<unsigned long>(tamanho / 1024U),
                static_cast<unsigned long>(
                    progresso.velocidade_upload_bytes_s / 1024U),
                static_cast<unsigned long>(
                    ((progresso.velocidade_upload_bytes_s % 1024U) * 10U) /
                    1024U),
                static_cast<unsigned long>(
                    progresso.tempo_restante_upload_s));
            proximo_relatorio_ms =
                instante_ms + configuracao::INTERVALO_RELATORIO_UPLOAD_MS;
        }
    }
    heap_caps_free(bloco);
    if (erro != ESP_OK) return erro;

    definir_aguardando_confirmacao_upload(true);
    servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
        "upload_csv", "confirmacao Google", 180000);
    const int64_t tamanho_resposta = esp_http_client_fetch_headers(cliente);
    if (tamanho_resposta < 0) {
        return tamanho_resposta == -ESP_ERR_HTTP_EAGAIN
                   ? ESP_ERR_HTTP_EAGAIN
                   : ESP_FAIL;
    }
    int bytes_descartados = 0;
    erro = esp_http_client_flush_response(cliente, &bytes_descartados);
    return erro;
}

esp_err_t enviar_arquivo(const char* nome, uint32_t indice,
                         uint32_t quantidade, uint32_t* tamanho_enviado) {
    servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
        "upload_csv", "abertura arquivo/DNS/TLS", 180000);
    char caminho[192]{};
    if (std::snprintf(caminho, sizeof(caminho), "%s/%s",
                      configuracao::DIRETORIO_TELEMETRIA, nome) >=
        static_cast<int>(sizeof(caminho))) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE* arquivo = nullptr;
    size_t tamanho = 0;
    esp_err_t erro = abrir_arquivo_upload(caminho, &arquivo, &tamanho);
    if (erro != ESP_OK) return erro;
    iniciar_progresso_arquivo(nome, tamanho);
    ESP_LOGI(ETIQUETA, "Enviando arquivo %lu de %lu: %s (%lu KiB)",
             static_cast<unsigned long>(indice),
             static_cast<unsigned long>(quantidade), nome,
             static_cast<unsigned long>(tamanho / 1024U));

    char url[768]{};
    erro = montar_url_upload(nome, url, sizeof(url));
    if (erro != ESP_OK) {
        std::fclose(arquivo);
        return erro;
    }

    RespostaHttp resposta{};
    esp_http_client_config_t http{};
    http.url = url;
    http.method = HTTP_METHOD_POST;
    http.timeout_ms = configuracao::TEMPO_LIMITE_UPLOAD_MS;
    http.buffer_size_tx = configuracao::TAMANHO_BUFFER_TRANSMISSAO_HTTP_BYTES;
    http.event_handler = tratar_evento_http;
    http.user_data = &resposta;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    // O ContentService executa o POST e redireciona a leitura da resposta.
    // O ESP-IDF preserva o método no 302, então a continuação precisa ser manual.
    http.disable_auto_redirect = true;
    esp_http_client_handle_t cliente = esp_http_client_init(&http);
    if (cliente == nullptr) {
        std::fclose(arquivo);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(cliente, "Content-Type",
                               "text/csv; charset=utf-8");
    erro = transmitir_arquivo(cliente, arquivo, tamanho);
    std::fclose(arquivo);
    const int codigo_post = esp_http_client_get_status_code(cliente);
    esp_http_client_cleanup(cliente);

    char confirmacao[128]{};
    std::snprintf(confirmacao, sizeof(confirmacao), "OK:%s", nome);
    if (erro == ESP_OK && codigo_post == 200 &&
        std::strcmp(resposta.texto, confirmacao) == 0) {
        if (tamanho_enviado != nullptr) {
            *tamanho_enviado = static_cast<uint32_t>(tamanho);
        }
        return ESP_OK;
    }
    const bool redirecionado =
        codigo_post == 301 || codigo_post == 302 || codigo_post == 303 ||
        codigo_post == 307 || codigo_post == 308;
    constexpr char HOST_RESPOSTA[] =
        "https://script.googleusercontent.com/";
    if (erro != ESP_OK || !redirecionado ||
        std::strncmp(resposta.redirecionamento, HOST_RESPOSTA,
                     sizeof(HOST_RESPOSTA) - 1) != 0) {
        ESP_LOGE(ETIQUETA,
                 "POST de %s falhou: transporte=%s HTTP=%d redirecionamento_google=%s resposta='%s'",
                 nome, esp_err_to_name(erro), codigo_post,
                 redirecionado ? "inesperado" : "ausente", resposta.texto);
        return erro != ESP_OK ? erro : ESP_ERR_INVALID_RESPONSE;
    }

    resposta.texto[0] = '\0';
    resposta.tamanho = 0;
    esp_http_client_config_t consulta{};
    consulta.url = resposta.redirecionamento;
    consulta.method = HTTP_METHOD_GET;
    consulta.timeout_ms = configuracao::TEMPO_LIMITE_UPLOAD_MS;
    consulta.buffer_size_tx = configuracao::TAMANHO_BUFFER_TRANSMISSAO_HTTP_BYTES;
    consulta.event_handler = tratar_evento_http;
    consulta.user_data = &resposta;
    consulta.crt_bundle_attach = esp_crt_bundle_attach;
    consulta.max_redirection_count = 2;
    cliente = esp_http_client_init(&consulta);
    if (cliente == nullptr) return ESP_ERR_NO_MEM;
    erro = esp_http_client_perform(cliente);
    const int codigo_http = esp_http_client_get_status_code(cliente);
    esp_http_client_cleanup(cliente);
    if (erro == ESP_OK && codigo_http == 200 &&
        std::strcmp(resposta.texto, confirmacao) == 0) {
        if (tamanho_enviado != nullptr) {
            *tamanho_enviado = static_cast<uint32_t>(tamanho);
        }
        return ESP_OK;
    }
    ESP_LOGE(ETIQUETA,
             "Upload de %s falhou: transporte=%s HTTP=%d resposta='%s'",
             nome, esp_err_to_name(erro), codigo_http, resposta.texto);
    return erro != ESP_OK ? erro : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t enviar_com_repeticao(const char* nome, uint32_t indice,
                               uint32_t quantidade,
                               uint32_t* tamanho_enviado) {
    esp_err_t ultimo_erro = ESP_FAIL;
    uint32_t espera_ms = configuracao::ESPERA_INICIAL_NOVA_TENTATIVA_MS;
    for (uint8_t tentativa = 0;
         tentativa < configuracao::MAXIMO_TENTATIVAS_UPLOAD; ++tentativa) {
        if (tentativa > 0) {
            ESP_LOGW(ETIQUETA, "Nova tentativa %u de %u para %s",
                     static_cast<unsigned>(tentativa + 1),
                     static_cast<unsigned>(
                         configuracao::MAXIMO_TENTATIVAS_UPLOAD),
                     nome);
        }
        ultimo_erro =
            enviar_arquivo(nome, indice, quantidade, tamanho_enviado);
        if (ultimo_erro == ESP_OK) return ESP_OK;
        definir_aguardando_confirmacao_upload(false);
        if (tentativa + 1 < configuracao::MAXIMO_TENTATIVAS_UPLOAD) {
            vTaskDelay(pdMS_TO_TICKS(espera_ms));
            espera_ms = std::min<uint32_t>(espera_ms * 2U, 8000U);
        }
    }
    return ultimo_erro;
}

esp_err_t mover_para_enviados(const char* nome) {
    char origem[192]{};
    char destino[192]{};
    if (std::snprintf(origem, sizeof(origem), "%s/%s",
                      configuracao::DIRETORIO_TELEMETRIA, nome) >=
            static_cast<int>(sizeof(origem)) ||
        std::snprintf(destino, sizeof(destino), "%s/%s",
                      configuracao::DIRETORIO_TELEMETRIA_ENVIADA, nome) >=
            static_cast<int>(sizeof(destino))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return rename(origem, destino) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t sincronizar_com_google(ResultadoSincronizacao* resultado) {
    if (resultado == nullptr) return ESP_ERR_INVALID_ARG;
    *resultado = {};
    resultado->arquivos_total = contar_arquivos_pendentes();
    portENTER_CRITICAL(&trava_estado);
    estado.arquivos_upload_total = resultado->arquivos_total;
    estado.arquivos_upload_concluidos = 0;
    estado.arquivo_atual_bytes_enviados = 0;
    estado.arquivo_atual_bytes_total = 0;
    estado.velocidade_upload_bytes_s = 0;
    estado.tempo_restante_upload_s = 0;
    estado.aguardando_confirmacao_upload = false;
    estado.arquivo_em_upload[0] = '\0';
    portEXIT_CRITICAL(&trava_estado);
    ESP_LOGI(ETIQUETA, "Sincronização iniciada: %lu arquivo(s) pendente(s)",
             static_cast<unsigned long>(resultado->arquivos_total));
    if (resultado->arquivos_total == 0) {
        ESP_LOGI(ETIQUETA, "Google já está sincronizado: nenhum CSV pendente");
        return ESP_OK;
    }

    esp_err_t erro = ESP_OK;
    if (configuracao::ENDERECO_GOOGLE_TELEMETRIA[0] == '\0' ||
        configuracao::TOKEN_GOOGLE_TELEMETRIA[0] == '\0') {
        ESP_LOGW(
            ETIQUETA,
            "Upload não configurado; defina TELEMETRIA_GOOGLE_URL e TELEMETRIA_GOOGLE_TOKEN");
        return ESP_ERR_INVALID_STATE;
    }

    erro = servico_wifi_abrir_sessao(
        FinalidadeSessaoWifi::SincronizacaoGoogle);
    if (erro != ESP_OK) {
        const ResumoWifi wifi = servico_wifi_obter_resumo();
        ESP_LOGE(ETIQUETA,
                 "Sincronização Google sem rede: erro=%s tentativas=%" PRIu32
                 " desconexao=%" PRId32 " ultimo_wifi=%s",
                 esp_err_to_name(erro), wifi.tentativas_ultima_sessao,
                 wifi.ultimo_motivo_desconexao, esp_err_to_name(wifi.ultimo_erro));
        return erro;
    }

    while (erro == ESP_OK) {
        char nome[96]{};
        DIR* diretorio = opendir(configuracao::DIRETORIO_TELEMETRIA);
        if (diretorio == nullptr) {
            erro = ESP_FAIL;
            break;
        }
        while (dirent* entrada = readdir(diretorio)) {
            if (termina_com(entrada->d_name, ".csv")) {
                std::strncpy(nome, entrada->d_name, sizeof(nome) - 1);
                break;
            }
        }
        closedir(diretorio);
        if (nome[0] == '\0') break;
        uint32_t tamanho_enviado = 0;
        erro = enviar_com_repeticao(
            nome, resultado->arquivos_enviados + 1,
            resultado->arquivos_total, &tamanho_enviado);
        if (erro == ESP_OK) {
            erro = mover_para_enviados(nome);
            if (erro != ESP_OK) {
                ESP_LOGE(ETIQUETA,
                         "Google confirmou %s, mas mover CSV no SD falhou: errno=%d",
                         nome, errno);
                break;
            }
            resultado->arquivos_enviados++;
            resultado->bytes_enviados += tamanho_enviado;
            portENTER_CRITICAL(&trava_estado);
            estado.arquivos_enviados++;
            estado.arquivos_upload_concluidos =
                resultado->arquivos_enviados;
            portEXIT_CRITICAL(&trava_estado);
            ESP_LOGI(
                ETIQUETA,
                "Upload confirmado: %s; %lu concluído(s), %lu restante(s)",
                nome,
                static_cast<unsigned long>(resultado->arquivos_enviados),
                static_cast<unsigned long>(
                    resultado->arquivos_total -
                    resultado->arquivos_enviados));
        }
    }

    const esp_err_t erro_wifi = servico_wifi_encerrar_sessao(
        FinalidadeSessaoWifi::SincronizacaoGoogle);
    if (erro == ESP_OK) erro = erro_wifi;
    atualizar_pendentes();
    return erro;
}

void tarefa_registro(void*) {
    // Só toca o SD depois que todas as tarefas da inicialização foram criadas.
    // Assim o rollback por falta de memória não interrompe uma gravação de log.
    while (!servico_registro_telemetria_obter_estado().iniciado) {
        vTaskDelay(1);
    }
    ComandoRegistro comando{};
    while (true) {
        servico_supervisao_informar_atividade(ModuloSupervisionado::RegistroSd,
            "registro_csv", "gravacao/diagnostico SD", 30000);
        atualizar_backup_configuracao(agora_ms());
        registrar_diagnostico_esp();
        // Verifica falhas mesmo sem novas amostras, com latência máxima de 500 ms.
        if (xQueueReceive(fila, &comando, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        esp_err_t erro = ESP_OK;
        if (comando.tipo == TipoComandoRegistro::Amostra) {
            erro = gravar_amostra(comando);
        } else {
            erro = fechar_arquivo_atual();
            portENTER_CRITICAL(&trava_estado);
            erro_preparacao_upload = erro;
            portEXIT_CRITICAL(&trava_estado);
            xSemaphoreGive(arquivo_pronto_para_upload);
        }
        definir_ultimo_erro(erro);
        if (erro != ESP_OK) {
            ESP_LOGE(ETIQUETA, "Operação de registro falhou: %s",
                     esp_err_to_name(erro));
        }
    }
}

void tarefa_upload(void*) {
    while (true) {
        servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
            "upload_csv", "aguardando solicitacao", 0);
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
            "upload_csv", "preparacao/rede", 180000);
        const uint32_t inicio_sincronizacao_ms = agora_ms();
        ResultadoSincronizacao resultado{};
        ComandoRegistro comando{};
        comando.tipo = TipoComandoRegistro::FecharArquivo;
        esp_err_t erro = xQueueSend(fila, &comando, pdMS_TO_TICKS(1000)) == pdTRUE
                             ? ESP_OK
                             : ESP_ERR_TIMEOUT;
        if (erro == ESP_OK &&
            xSemaphoreTake(arquivo_pronto_para_upload,
                           pdMS_TO_TICKS(10000)) != pdTRUE) {
            erro = ESP_ERR_TIMEOUT;
        }
        if (erro == ESP_OK) {
            portENTER_CRITICAL(&trava_estado);
            erro = erro_preparacao_upload;
            portEXIT_CRITICAL(&trava_estado);
        }
        if (erro == ESP_OK) erro = sincronizar_com_google(&resultado);
        definir_ultimo_erro(erro);
        const uint32_t duracao_ms = agora_ms() - inicio_sincronizacao_ms;
        const uint32_t velocidade_media = duracao_ms == 0
            ? 0
            : static_cast<uint32_t>(
                  (static_cast<uint64_t>(resultado.bytes_enviados) * 1000U) /
                  duracao_ms);
        portENTER_CRITICAL(&trava_estado);
        estado.sincronizacao_em_andamento = false;
        estado.aguardando_confirmacao_upload = false;
        estado.sincronizacoes_finalizadas++;
        estado.arquivos_ultima_sincronizacao = resultado.arquivos_enviados;
        estado.bytes_ultima_sincronizacao = resultado.bytes_enviados;
        estado.duracao_ultima_sincronizacao_ms = duracao_ms;
        estado.fim_ultima_sincronizacao_ms = agora_ms();
        estado.ultimo_resultado_sincronizacao = erro;
        portEXIT_CRITICAL(&trava_estado);
        ESP_LOGI(
            ETIQUETA,
            "Resumo da sincronização: %lu/%lu arquivo(s), %lu KiB em %lu.%lu s, média %lu.%lu KiB/s, resultado=%s",
            static_cast<unsigned long>(resultado.arquivos_enviados),
            static_cast<unsigned long>(resultado.arquivos_total),
            static_cast<unsigned long>(resultado.bytes_enviados / 1024U),
            static_cast<unsigned long>(duracao_ms / 1000U),
            static_cast<unsigned long>((duracao_ms % 1000U) / 100U),
            static_cast<unsigned long>(velocidade_media / 1024U),
            static_cast<unsigned long>(
                ((velocidade_media % 1024U) * 10U) / 1024U),
            esp_err_to_name(erro));
        if (erro != ESP_OK) {
            ESP_LOGE(ETIQUETA, "Sincronização com o Google falhou: %s",
                     esp_err_to_name(erro));
        }
    }
}
}  // namespace

esp_err_t servico_registro_telemetria_iniciar() {
    if (!configuracao::HABILITAR_REGISTRO_TELEMETRIA) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (estado.iniciado) return ESP_OK;
    // A amostragem RAM continua mesmo se o SD falhar e o início for repetido.
    iniciar_historico_ram();

    ConfiguracaoCartaoSd sd{};
    sd.ponto_montagem = configuracao::PONTO_MONTAGEM_SD;
    sd.pino_clk = configuracao::PINO_SD_CLK;
    sd.pino_mosi = configuracao::PINO_SD_MOSI;
    sd.pino_miso = configuracao::PINO_SD_MISO;
    sd.pino_cs = configuracao::PINO_SD_CS;
    sd.frequencia_hz = configuracao::FREQUENCIA_SD_HZ;
    esp_err_t erro = gerenciador_cartao_sd_iniciar(sd);
    if (erro != ESP_OK) return erro;
    erro = criar_diretorio(configuracao::DIRETORIO_TELEMETRIA);
    if (erro == ESP_OK) {
        erro = criar_diretorio(configuracao::DIRETORIO_TELEMETRIA_ENVIADA);
    }
    if (erro != ESP_OK) return erro;

    (void)recuperar_arquivos_interrompidos();
    if (criar_diretorio(configuracao::DIRETORIO_LOG_ESP) == ESP_OK) {
        iniciar_sequencia_logs();
    }
    recuperar_backup_configuracao();
    sessao = esp_random();
    fila = xQueueCreate(configuracao::CAPACIDADE_FILA_REGISTRO,
                        sizeof(ComandoRegistro));
    arquivo_pronto_para_upload = xSemaphoreCreateBinary();
    if (fila == nullptr || arquivo_pronto_para_upload == nullptr) {
        if (fila != nullptr) vQueueDelete(fila);
        if (arquivo_pronto_para_upload != nullptr)
            vSemaphoreDelete(arquivo_pronto_para_upload);
        fila = nullptr;
        arquivo_pronto_para_upload = nullptr;
        return ESP_ERR_NO_MEM;
    }
    tarefas_log = static_cast<DiagnosticoTarefas*>(heap_caps_calloc(
        1, sizeof(DiagnosticoTarefas), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (configuracao::criar_tarefa(tarefa_registro, "registro_csv", nullptr, &tarefa_registro_handle) != pdPASS ||
        configuracao::criar_tarefa(tarefa_upload, "upload_csv", nullptr, &tarefa_upload_handle) != pdPASS) {
        if (tarefa_registro_handle != nullptr) {
            vTaskDelete(tarefa_registro_handle);
            tarefa_registro_handle = nullptr;
        }
        if (tarefa_upload_handle != nullptr) {
            vTaskDelete(tarefa_upload_handle);
            tarefa_upload_handle = nullptr;
        }
        heap_caps_free(tarefas_log);
        tarefas_log = nullptr;
        vQueueDelete(fila);
        vSemaphoreDelete(arquivo_pronto_para_upload);
        fila = nullptr;
        arquivo_pronto_para_upload = nullptr;
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&trava_estado);
    estado.iniciado = true;
    estado.cartao_disponivel = true;
    estado.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava_estado);
    atualizar_pendentes();
    ESP_LOGI(ETIQUETA, "Histórico ativo a 1 Hz; upload somente sob demanda");
    return ESP_OK;
}

esp_err_t servico_registro_telemetria_registrar(
    const DadosTelemetriaVeiculo& dados) {
    if (!estado.iniciado || fila == nullptr) return ESP_ERR_INVALID_STATE;
    const uint32_t instante = agora_ms();
    portENTER_CRITICAL(&trava_estado);
    const bool deve_registrar =
        proximo_registro_ms == 0 ||
        static_cast<int32_t>(instante - proximo_registro_ms) >= 0;
    if (deve_registrar) {
        proximo_registro_ms =
            instante + configuracao::INTERVALO_REGISTRO_TELEMETRIA_MS;
    }
    portEXIT_CRITICAL(&trava_estado);
    if (!deve_registrar) return ESP_OK;

    ComandoRegistro comando{};
    comando.tipo = TipoComandoRegistro::Amostra;
    comando.tempo_sessao_ms = instante;
    comando.dados = dados;
    if (xQueueSend(fila, &comando, 0) != pdTRUE) {
        incrementar_descartadas();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t servico_registro_telemetria_solicitar_sincronizacao() {
    if (!estado.iniciado || tarefa_upload_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    if (wifi.sessao_em_andamento) {
        ESP_LOGW(
            ETIQUETA,
            "Sincronização recusada: Wi-Fi ocupado pela %s",
            servico_wifi_nome_finalidade_sessao(wifi.finalidade_sessao));
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&trava_estado);
    if (estado.sincronizacao_em_andamento) {
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_INVALID_STATE;
    }
    estado.sincronizacao_em_andamento = true;
    estado.sincronizacoes_iniciadas++;
    estado.aguardando_confirmacao_upload = false;
    estado.arquivos_upload_total = 0;
    estado.arquivos_upload_concluidos = 0;
    estado.arquivo_atual_bytes_enviados = 0;
    estado.arquivo_atual_bytes_total = 0;
    estado.velocidade_upload_bytes_s = 0;
    estado.tempo_restante_upload_s = 0;
    estado.arquivo_em_upload[0] = '\0';
    portEXIT_CRITICAL(&trava_estado);
    xTaskNotifyGive(tarefa_upload_handle);
    return ESP_OK;
}

EstadoRegistroTelemetria servico_registro_telemetria_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoRegistroTelemetria copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}

void servico_registro_telemetria_acertar_relogio_http(const char* date_header) {
    acertar_relogio_http_interno(date_header);
}
