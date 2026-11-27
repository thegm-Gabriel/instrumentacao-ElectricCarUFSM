// Preserva a configuração do Mestre em um arquivo verificado no cartão SD.
#include "servicos/historico/backup_configuracao.h"

#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "esp_log.h"
#include "nucleo/configuracao/historico.h"
#include "servicos/comunicacao/wifi_sessoes.h"
#include "servicos/interface_local/relatorios.h"
#include "servicos/manutencao/ota.h"
#include "servicos/veiculo/odometro.h"

namespace backup_configuracao {
namespace {
constexpr char ETIQUETA[] = "registro_telemetria";
uint32_t ultimo_backup_ms = 0;
uint32_t ultima_tentativa_backup_falha_ms = 0;
bool aguarda_nova_tentativa_backup = false;
uint32_t sequencia_backup = 0;
uint64_t ultimo_odometro_backup_mm = UINT64_MAX;

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
}  // namespace

void recuperar() {
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

void atualizar(uint32_t instante) {
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

}  // namespace backup_configuracao
