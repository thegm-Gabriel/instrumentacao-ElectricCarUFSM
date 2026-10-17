// Coordena a fila de amostras, arquivos CSV recuperáveis e upload HTTPS em lote.
#include "nucleo/configuracao_tarefas.h"
#include "servicos/servico_registro_telemetria.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
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
uint32_t segmento_log = 0;
uint32_t ultimo_log_ms = 0;
uint32_t ultimo_log_tarefas_ms = 0;
uint32_t repetir_log_ms = 0;
size_t bytes_log_sessao = 0;
bool log_limite_avisado = false;
ResumoSaudeSistema ultimo_log_saude{};
uint32_t ultimo_log_sincronizacoes = 0;
uint32_t ultimo_log_descartes = 0;
esp_err_t ultimo_log_erro_csv = ESP_OK;
DiagnosticoTarefas* tarefas_log = nullptr;
esp_err_t criar_diretorio(const char* caminho);

void registrar_diagnostico_esp() {
    const uint32_t instante = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (repetir_log_ms != 0 && static_cast<int32_t>(instante - repetir_log_ms) < 0) return;
    if (bytes_log_sessao >= configuracao::LIMITE_LOG_ESP_POR_SESSAO) {
        if (!log_limite_avisado) ESP_LOGW(ETIQUETA, "Limite do log ESP nesta sessão atingido; CSV continua ativo");
        log_limite_avisado = true;
        return;
    }
    const auto saude = servico_diagnostico_obter_resumo();
    if (!saude.valido) return;
    // Agrega rajadas de erros para não disputar o SD a cada amostra.
    if (ultimo_log_ms != 0 && instante - ultimo_log_ms < 1000) return;
    const auto registro = servico_registro_telemetria_obter_estado();
    const bool mudou = saude.causas_ativas != ultimo_log_saude.causas_ativas ||
        saude.erros_sensores != ultimo_log_saude.erros_sensores ||
        saude.erros_i2c != ultimo_log_saude.erros_i2c ||
        saude.erros_uart != ultimo_log_saude.erros_uart ||
        saude.falhas_ota != ultimo_log_saude.falhas_ota ||
        saude.falhas_supervisao != ultimo_log_saude.falhas_supervisao ||
        saude.wifi_conectado != ultimo_log_saude.wifi_conectado ||
        registro.sincronizacoes_finalizadas != ultimo_log_sincronizacoes ||
        registro.amostras_descartadas != ultimo_log_descartes ||
        registro.ultimo_erro != ultimo_log_erro_csv;
    if (ultimo_log_ms != 0 && !mudou &&
        instante - ultimo_log_ms < configuracao::INTERVALO_LOG_ESP_MS) return;

    FILE* arquivo = nullptr;
    bool novo = caminho_log[0] == '\0';
    if (!novo) {
        struct stat info{};
        if (stat(caminho_log, &info) != 0) goto falhou;
        novo = static_cast<size_t>(info.st_size) >= configuracao::TAMANHO_SEGMENTO_LOG_ESP;
    }
    if (novo) {
        if (criar_diretorio(configuracao::DIRETORIO_LOG_ESP) != ESP_OK) goto falhou;
        // Exclusivo: um identificador de sessão repetido nunca sobrescreve logs.
        for (unsigned tentativa = 0; tentativa < 32 && arquivo == nullptr; ++tentativa) {
            std::snprintf(caminho_log, sizeof(caminho_log), "%s/esp_%08" PRIx32 "_%04" PRIu32 ".log",
                          configuracao::DIRETORIO_LOG_ESP, sessao, segmento_log++);
            arquivo = std::fopen(caminho_log, "wx");
            if (arquivo == nullptr && errno != EEXIST) break;
        }
    } else {
        arquivo = std::fopen(caminho_log, "a");
    }
    if (arquivo == nullptr) goto falhou;
    // Em modo append, a posição inicial do stream pode ser zero. Posiciona
    // explicitamente no fim para contabilizar só os bytes deste registro.
    if (std::fseek(arquivo, 0, SEEK_END) != 0) goto falhou;
    {
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
            std::fprintf(arquivo, "Tempos em ms desde boot; cpu_decimos: 1000=um nucleo; nucleo=afinidade (-1=livre); estado=FreeRTOS eTaskState. Log estruturado, sem credenciais.\n");
        }
        std::fprintf(arquivo, "ESTADO ms=%" PRIu32 " saude=%s causas=0x%" PRIx32 " interno=%" PRIu32 " bloco=%" PRIu32 " psram=%" PRIu32 " heap_min=%" PRIu32 " pilha_min=%" PRIu32 " tarefa_pilha=%s dma=%u dma_bloco=%u\n",
                     instante, servico_diagnostico_nome_estado(saude.estado), saude.causas_ativas,
                     saude.heap_interno_livre, saude.maior_bloco_heap_interno, saude.psram_livre,
                     saude.menor_heap_livre, saude.bytes_pilha_livres,
                     saude.tarefa_menor_pilha,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
        std::fprintf(arquivo, "ERROS sensores=%" PRIu32 " i2c=%" PRIu32 " uart=%" PRIu32 " descartes_uart=%" PRIu32 " ota=%" PRIu32 " supervisao=%" PRIu32 " csv=%s descartes_csv=%" PRIu32 "\n",
                     saude.erros_sensores, saude.erros_i2c, saude.erros_uart, saude.descartes_uart,
                     saude.falhas_ota, saude.falhas_supervisao, esp_err_to_name(registro.ultimo_erro), registro.amostras_descartadas);
        std::fprintf(arquivo, "UPLOAD ativo=%d confirmando=%d concluidos=%" PRIu32 "/%" PRIu32 " pendentes=%" PRIu32 " ultimo=%s wifi=%d rssi=%d\n",
                     registro.sincronizacao_em_andamento, registro.aguardando_confirmacao_upload,
                     registro.arquivos_upload_concluidos, registro.arquivos_upload_total,
                     registro.arquivos_pendentes, esp_err_to_name(registro.ultimo_resultado_sincronizacao),
                     saude.wifi_conectado, saude.wifi_rssi_dbm);
        const auto sensores = servico_diagnostico_obter_sensores();
        std::fprintf(arquivo, "SENSORES adc=%s mpu=%s encoder=%s odometro_nvs=%s\n",
                     esp_err_to_name(sensores.adc.ultimo_erro), esp_err_to_name(sensores.mpu6050.ultimo_erro),
                     esp_err_to_name(sensores.encoder.ultimo_erro), esp_err_to_name(sensores.persistencia_odometro.ultimo_erro));
        for (unsigned i = 0; i < static_cast<unsigned>(ModuloSupervisionado::Quantidade); ++i) {
            const auto modulo = servico_supervisao_obter_estado(static_cast<ModuloSupervisionado>(i));
            if (!modulo.registrado) continue;
            std::fprintf(arquivo, "SUP %s fase=%s idade_ms=%" PRIu32 " limite_ms=%" PRIu32 " atrasado=%d recuperando=%d falhas=%" PRIu32 " ultimo=%s\n",
                         modulo.nome, modulo.fase, modulo.idade_ultimo_pulso_ms,
                         modulo.tempo_limite_ms, modulo.atrasado, modulo.em_recuperacao,
                         modulo.falhas_detectadas, esp_err_to_name(modulo.ultimo_resultado));
        }
        if (novo || ultimo_log_tarefas_ms == 0 || instante - ultimo_log_tarefas_ms >= configuracao::INTERVALO_LOG_ESP_MS) {
            if (tarefas_log != nullptr) {
                servico_diagnostico_obter_tarefas(tarefas_log);
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
    }
    ultimo_log_ms = instante;
    ultimo_log_saude = saude;
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
        const int escritos = esp_http_client_write(
            cliente, bloco, static_cast<int>(lidos));
        if (escritos != static_cast<int>(lidos)) {
            erro = ESP_ERR_HTTP_WRITE_DATA;
            break;
        }
        enviados += static_cast<size_t>(escritos);
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
    if (resultado->arquivos_total == 0) return ESP_OK;

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
    if (erro != ESP_OK) return erro;

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
            if (erro != ESP_OK) break;
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
        registrar_diagnostico_esp();
        if (xQueueReceive(fila, &comando, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
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
