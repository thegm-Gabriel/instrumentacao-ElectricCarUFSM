#include "servicos/comunicacao/relogio_http.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <strings.h>
#include <sys/time.h>

#include "esp_log.h"

namespace {
constexpr char ETIQUETA[] = "registro_telemetria";
constexpr std::time_t UTC_MINIMA_VALIDA = 1704067200;  // 2024-01-01 UTC
}  // namespace

std::time_t relogio_utc_obter() {
    const std::time_t utc = std::time(nullptr);
    return utc >= UTC_MINIMA_VALIDA ? utc : 0;
}

void relogio_http_processar_cabecalho(const char* chave, const char* valor) {
    if (chave == nullptr || valor == nullptr || strcasecmp(chave, "Date") != 0 ||
        relogio_utc_obter() != 0) return;
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
    if (epoch < UTC_MINIMA_VALIDA) return;
    timeval instante{};
    instante.tv_sec = epoch;
    if (settimeofday(&instante, nullptr) == 0) {
        ESP_LOGI(ETIQUETA, "Relógio UTC sincronizado com cabeçalho HTTPS Date");
    }
}
