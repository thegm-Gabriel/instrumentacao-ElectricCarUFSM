// Coordena a fila de amostras, arquivos CSV recuperáveis e upload HTTPS em lote.
#include "servicos/servico_registro_telemetria.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_cartao_sd.h"
#include "nucleo/configuracao_registro_telemetria.h"
#include "servicos/servico_wifi.h"

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

enum class TipoComandoRegistro : uint8_t { Amostra, Sincronizar };

struct ComandoRegistro {
    TipoComandoRegistro tipo = TipoComandoRegistro::Amostra;
    uint32_t tempo_sessao_ms = 0;
    DadosTelemetriaVeiculo dados{};
};

struct RespostaHttp {
    char texto[160]{};
    size_t tamanho = 0;
};

QueueHandle_t fila = nullptr;
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
        if (escritos_origem <= 0 ||
            escritos_origem >= static_cast<int>(sizeof(origem)) ||
            escritos_destino <= 0 ||
            escritos_destino >= static_cast<int>(sizeof(destino)) ||
            rename(origem, destino) != 0) {
            resultado = ESP_FAIL;
            ESP_LOGE(ETIQUETA,
                     "Não foi possível recuperar o arquivo temporário %s",
                     entrada->d_name);
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
    if (std::fwrite(linha, 1, tamanho, arquivo_atual) != tamanho) return ESP_FAIL;

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
    if (evento->event_id != HTTP_EVENT_ON_DATA || evento->data == nullptr ||
        evento->data_len <= 0) {
        return ESP_OK;
    }
    auto* resposta = static_cast<RespostaHttp*>(evento->user_data);
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

esp_err_t carregar_arquivo(const char* caminho, char** conteudo,
                           size_t* tamanho) {
    FILE* arquivo = std::fopen(caminho, "rb");
    if (arquivo == nullptr) return ESP_ERR_NOT_FOUND;
    if (std::fseek(arquivo, 0, SEEK_END) != 0) {
        std::fclose(arquivo);
        return ESP_FAIL;
    }
    const long bytes = std::ftell(arquivo);
    if (bytes <= 0 ||
        static_cast<size_t>(bytes) >
            configuracao::TAMANHO_MAXIMO_CSV_BYTES ||
        std::fseek(arquivo, 0, SEEK_SET) != 0) {
        std::fclose(arquivo);
        return ESP_ERR_INVALID_SIZE;
    }
    char* buffer = static_cast<char*>(heap_caps_malloc(
        static_cast<size_t>(bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        buffer = static_cast<char*>(
            heap_caps_malloc(static_cast<size_t>(bytes), MALLOC_CAP_8BIT));
    }
    if (buffer == nullptr) {
        std::fclose(arquivo);
        return ESP_ERR_NO_MEM;
    }
    const size_t lidos =
        std::fread(buffer, 1, static_cast<size_t>(bytes), arquivo);
    std::fclose(arquivo);
    if (lidos != static_cast<size_t>(bytes)) {
        heap_caps_free(buffer);
        return ESP_FAIL;
    }
    *conteudo = buffer;
    *tamanho = lidos;
    return ESP_OK;
}

esp_err_t enviar_arquivo(const char* nome) {
    char caminho[192]{};
    if (std::snprintf(caminho, sizeof(caminho), "%s/%s",
                      configuracao::DIRETORIO_TELEMETRIA, nome) >=
        static_cast<int>(sizeof(caminho))) {
        return ESP_ERR_INVALID_SIZE;
    }
    char* conteudo = nullptr;
    size_t tamanho = 0;
    esp_err_t erro = carregar_arquivo(caminho, &conteudo, &tamanho);
    if (erro != ESP_OK) return erro;

    char url[768]{};
    erro = montar_url_upload(nome, url, sizeof(url));
    if (erro != ESP_OK) {
        heap_caps_free(conteudo);
        return erro;
    }

    RespostaHttp resposta{};
    esp_http_client_config_t http{};
    http.url = url;
    http.method = HTTP_METHOD_POST;
    http.timeout_ms = configuracao::TEMPO_LIMITE_UPLOAD_MS;
    http.buffer_size_tx = 4096;
    http.event_handler = tratar_evento_http;
    http.user_data = &resposta;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.max_redirection_count = 5;
    esp_http_client_handle_t cliente = esp_http_client_init(&http);
    if (cliente == nullptr) {
        heap_caps_free(conteudo);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(cliente, "Content-Type",
                               "text/csv; charset=utf-8");
    esp_http_client_set_post_field(cliente, conteudo,
                                   static_cast<int>(tamanho));
    erro = esp_http_client_perform(cliente);
    const int codigo_http = esp_http_client_get_status_code(cliente);
    esp_http_client_cleanup(cliente);
    heap_caps_free(conteudo);

    char confirmacao[128]{};
    std::snprintf(confirmacao, sizeof(confirmacao), "OK:%s", nome);
    if (erro == ESP_OK && codigo_http == 200 &&
        std::strcmp(resposta.texto, confirmacao) == 0) {
        return ESP_OK;
    }
    ESP_LOGE(ETIQUETA,
             "Upload de %s falhou: transporte=%s HTTP=%d resposta='%s'",
             nome, esp_err_to_name(erro), codigo_http, resposta.texto);
    return erro != ESP_OK ? erro : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t enviar_com_repeticao(const char* nome) {
    esp_err_t ultimo_erro = ESP_FAIL;
    uint32_t espera_ms = configuracao::ESPERA_INICIAL_NOVA_TENTATIVA_MS;
    for (uint8_t tentativa = 0;
         tentativa < configuracao::MAXIMO_TENTATIVAS_UPLOAD; ++tentativa) {
        ultimo_erro = enviar_arquivo(nome);
        if (ultimo_erro == ESP_OK) return ESP_OK;
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

esp_err_t sincronizar_com_google() {
    esp_err_t erro = fechar_arquivo_atual();
    if (erro != ESP_OK) return erro;
    if (configuracao::ENDERECO_GOOGLE_TELEMETRIA[0] == '\0' ||
        configuracao::TOKEN_GOOGLE_TELEMETRIA[0] == '\0') {
        ESP_LOGW(
            ETIQUETA,
            "Upload não configurado; defina TELEMETRIA_GOOGLE_URL e TELEMETRIA_GOOGLE_TOKEN");
        return ESP_ERR_INVALID_STATE;
    }

    const ResumoWifi wifi_antes = servico_wifi_obter_resumo();
    const bool sessao_propria = !wifi_antes.conectado;
    if (sessao_propria) {
        erro = servico_wifi_abrir_sessao();
        if (erro != ESP_OK) return erro;
    }

    DIR* diretorio = opendir(configuracao::DIRETORIO_TELEMETRIA);
    if (diretorio == nullptr) {
        erro = ESP_FAIL;
    } else {
        while (dirent* entrada = readdir(diretorio)) {
            if (!termina_com(entrada->d_name, ".csv")) continue;
            erro = enviar_com_repeticao(entrada->d_name);
            if (erro != ESP_OK) break;
            erro = mover_para_enviados(entrada->d_name);
            if (erro != ESP_OK) break;
            portENTER_CRITICAL(&trava_estado);
            estado.arquivos_enviados++;
            portEXIT_CRITICAL(&trava_estado);
            ESP_LOGI(ETIQUETA, "Upload confirmado: %s", entrada->d_name);
        }
        closedir(diretorio);
    }

    if (sessao_propria) {
        const esp_err_t erro_wifi = servico_wifi_encerrar_sessao();
        if (erro == ESP_OK) erro = erro_wifi;
    }
    atualizar_pendentes();
    return erro;
}

void tarefa_registro(void*) {
    ComandoRegistro comando{};
    while (true) {
        if (xQueueReceive(fila, &comando, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t erro = ESP_OK;
        if (comando.tipo == TipoComandoRegistro::Amostra) {
            erro = gravar_amostra(comando);
        } else {
            portENTER_CRITICAL(&trava_estado);
            estado.sincronizacao_em_andamento = true;
            portEXIT_CRITICAL(&trava_estado);
            erro = sincronizar_com_google();
            portENTER_CRITICAL(&trava_estado);
            estado.sincronizacao_em_andamento = false;
            portEXIT_CRITICAL(&trava_estado);
        }
        definir_ultimo_erro(erro);
        if (erro != ESP_OK) {
            ESP_LOGE(ETIQUETA, "Operação de registro falhou: %s",
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
    if (fila == nullptr) return ESP_ERR_NO_MEM;
    if (xTaskCreate(tarefa_registro, "registro_csv", 10240, nullptr, 3,
                    nullptr) != pdPASS) {
        vQueueDelete(fila);
        fila = nullptr;
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
    if (!estado.iniciado || fila == nullptr) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&trava_estado);
    if (estado.sincronizacao_em_andamento) {
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_INVALID_STATE;
    }
    estado.sincronizacao_em_andamento = true;
    portEXIT_CRITICAL(&trava_estado);
    ComandoRegistro comando{};
    comando.tipo = TipoComandoRegistro::Sincronizar;
    if (xQueueSend(fila, &comando, 0) == pdTRUE) return ESP_OK;
    portENTER_CRITICAL(&trava_estado);
    estado.sincronizacao_em_andamento = false;
    portEXIT_CRITICAL(&trava_estado);
    return ESP_ERR_NO_MEM;
}

EstadoRegistroTelemetria servico_registro_telemetria_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoRegistroTelemetria copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}
