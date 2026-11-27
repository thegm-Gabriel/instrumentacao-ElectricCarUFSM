// Apresenta e controla o submenu de sensores do terminal serial.
#include "servicos/interface_local/terminal/interno.h"

#include <cstdint>
#include <cstdio>

#include "dispositivos/entradas/ina228.h"
#include "esp_log.h"
#include "gerenciadores/i2c.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/interface.h"
#include "nucleo/configuracao/veiculo.h"
#include "servicos/manutencao/diagnostico.h"
#include "servicos/veiculo/aceleracao.h"
#include "servicos/veiculo/aquisicao.h"
#include "servicos/veiculo/odometro.h"
#include "servicos/veiculo/simulador.h"

namespace terminal_sensores {
namespace {
using terminal_console::imprimir_prompt;
using terminal_console::imprimir_titulo;
using terminal_console::solicitar_resumo;

constexpr char ETIQUETA[] = "terminal";

enum class TelaSensores : uint8_t {
    Principal,
    Controle,
    Adc,
    Mpu6050,
    Encoders,
    I2c,
    Ina228,
};

TelaSensores tela_atual = TelaSensores::Principal;

void imprimir_rodape_monitor_sensor() {
    std::printf("\r\n  Atualização automática a cada %lu ms | [Enter] Voltar aos sensores\r\n",
                static_cast<unsigned long>(
                    configuracao::INTERVALO_MONITOR_SENSORES_MS));
}

void mostrar_menu_sensores() {
    const RetratoDiagnosticoSistema retrato =
        servico_diagnostico_obter_retrato();
    const auto& dados = retrato.telemetria_atual;
    const auto& telemetria = retrato.telemetria;
    const auto& sensores = retrato.sensores;
    const auto& saude = retrato.saude;
    const auto& encoder_esquerdo = retrato.encoder_esquerdo;
    const auto& encoder_direito = retrato.encoder_direito;
    const auto& odometro = retrato.odometro;
    const auto& i2c = retrato.i2c;
    const EstadoServicoAceleracao aceleracao =
        servico_aceleracao_obter_estado();

    imprimir_titulo("SENSORES E DIAGNÓSTICO");
    std::printf("Fonte configurada : %s\r\n",
                configuracao::USAR_TELEMETRIA_SIMULADA
                    ? "simulador"
                    : "sensores reais");
    std::printf("Fonte atual       : %s\r\n",
                servico_simulador_telemetria_esta_ativo()
                    ? "simulador"
                    : "sensores reais");
    std::printf("Leitura permitida : ADC=%s | MPU=%s | encoders=%s | INA=%s\r\n",
                servico_aquisicao_sensor_ativo(SensorControlado::Adc) ? "sim" : "não",
                servico_aquisicao_sensor_ativo(SensorControlado::Mpu6050) ? "sim" : "não",
                servico_aquisicao_sensor_ativo(SensorControlado::Encoders) ? "sim" : "não",
                servico_aquisicao_sensor_ativo(SensorControlado::Ina228) ? "sim" : "não");
    std::printf("                   Temperatura ESP=%s\r\n",
                servico_aquisicao_sensor_ativo(SensorControlado::TemperaturaEsp) ? "sim" : "não");
    std::printf("Saúde do mestre   : %s | causas=0x%03lX\r\n",
                servico_diagnostico_nome_estado(saude.estado),
                static_cast<unsigned long>(saude.causas_ativas));
    std::printf("Amostras enviadas : %lu\r\n\r\n",
                static_cast<unsigned long>(telemetria.pacotes_gerados));

    if (telemetria.pacotes_gerados == 0) {
        std::printf("LEITURAS: aguardando a primeira amostra de telemetria.\r\n\r\n");
    } else {
        std::printf("LEITURAS MAIS RECENTES\r\n");
        if (servico_aquisicao_sensor_ativo(SensorControlado::Adc))
            std::printf("  ADC tensão       : %u de 4095\r\n",
                        static_cast<unsigned>(dados.tensao_adc_bruta));
        else
            std::printf("  ADC tensão       : PAUSADO\r\n");
        if (servico_aquisicao_sensor_ativo(SensorControlado::Mpu6050)) {
        std::printf("  Acelerômetro XYZ : %d, %d, %d\r\n",
                    dados.aceleracao_x, dados.aceleracao_y, dados.aceleracao_z);
        std::printf("  Aceleração XYZ   : %.3f, %.3f, %.3f g\r\n",
                    dados.aceleracao_x_g, dados.aceleracao_y_g,
                    dados.aceleracao_z_g);
        std::printf("  Acel. longitudinal: %+.3f g | %+.3f m/s²\r\n",
                    dados.aceleracao_g, dados.aceleracao_ms2);
        std::printf("  Calibração MPU   : %s | grav. longitudinal=%+.5f g | amostras=%u/%u\r\n",
                    aceleracao.calibrado ? "concluída" : "aguardando repouso",
                    aceleracao.componente_gravidade_longitudinal_g,
                    static_cast<unsigned>(
                        aceleracao.amostras_calibracao_coletadas),
                    static_cast<unsigned>(
                        configuracao::AMOSTRAS_CALIBRACAO_MPU6050));
        std::printf("  Giroscópio XYZ   : %d, %d, %d\r\n",
                    dados.giroscopio_x, dados.giroscopio_y, dados.giroscopio_z);
        } else {
            std::printf("  MPU6050          : PAUSADO\r\n");
        }
        if (servico_aquisicao_sensor_ativo(SensorControlado::Encoders))
            std::printf("  Velocidade       : %.2f km/h\r\n", dados.velocidade_kmh);
        else
            std::printf("  Velocidade       : leitura dos encoders PAUSADA\r\n");
        std::printf("  Percurso         : %.3f km\r\n", dados.percurso_km);
        std::printf("  Odômetro         : %.3f km\r\n\r\n", dados.odometro_km);
        if (dados.simulado) {
            std::printf("  Elétrica         : %.2f V | %+.3f A | %+.1f W (simulação)\r\n\r\n",
                        dados.tensao_pacote_v, dados.corrente_a, dados.potencia_w);
        } else if (!servico_aquisicao_sensor_ativo(SensorControlado::Ina228)) {
            std::printf("  INA228           : PAUSADO\r\n\r\n");
        } else if (dados.eletrica_valida) {
            std::printf("  INA228            : %.2f V | %+.3f A | %+.1f W\r\n\r\n",
                        dados.tensao_pacote_v, dados.corrente_a, dados.potencia_w);
        } else {
            std::printf("  INA228            : sem leitura elétrica válida\r\n\r\n");
        }
    }

    std::printf("ENCODERS / PCNT\r\n");
    if (!servico_aquisicao_sensor_ativo(SensorControlado::Encoders))
        std::printf("  Leitura pausada: valores abaixo são o último retrato; PCNT continua contando.\r\n");
    std::printf("  Esquerdo         : %s | GPIO A=%d B=%d | PCNT=%llu\r\n",
                encoder_esquerdo.inicializado ? "ativo" : "inativo",
                encoder_esquerdo.pino_a, encoder_esquerdo.pino_b,
                static_cast<unsigned long long>(encoder_esquerdo.contagem_atual));
    std::printf("    percurso/vel.  : %.3f m | %.2f km/h | resolução=%.6f m/contagem\r\n",
                odometro.roda_esquerda.percurso_m,
                odometro.roda_esquerda.velocidade_kmh,
                odometro.roda_esquerda.metros_por_contagem);
    std::printf("    contagens      : %llu | leituras/erros=%lu/%lu\r\n",
                static_cast<unsigned long long>(
                    odometro.roda_esquerda.contagens_acumuladas),
                static_cast<unsigned long>(encoder_esquerdo.leituras_ok),
                static_cast<unsigned long>(encoder_esquerdo.erros_leitura));
    std::printf("  Direito          : %s | GPIO A=%d B=%d | PCNT=%llu\r\n",
                encoder_direito.inicializado ? "ativo" : "inativo",
                encoder_direito.pino_a, encoder_direito.pino_b,
                static_cast<unsigned long long>(encoder_direito.contagem_atual));
    std::printf("    percurso/vel.  : %.3f m | %.2f km/h | resolução=%.6f m/contagem\r\n",
                odometro.roda_direita.percurso_m,
                odometro.roda_direita.velocidade_kmh,
                odometro.roda_direita.metros_por_contagem);
    std::printf("    contagens      : %llu | leituras/erros=%lu/%lu\r\n",
                static_cast<unsigned long long>(
                    odometro.roda_direita.contagens_acumuladas),
                static_cast<unsigned long>(encoder_direito.leituras_ok),
                static_cast<unsigned long>(encoder_direito.erros_leitura));
    std::printf("  Média das rodas  : %.3f m | %.2f km/h\r\n",
                odometro.percurso_m, odometro.velocidade_kmh);
    std::printf("  Coerência rodas  : %s | alertas=%lu | validações rejeitadas=%lu\r\n",
                odometro.rodas_coerentes ? "normal" : "VERIFICAR ENCODERS",
                static_cast<unsigned long>(odometro.divergencias_detectadas),
                static_cast<unsigned long>(odometro.erros_validacao));
    std::printf("  Persistência NVS : %s%s | falhas=%lu | intervalo=%.1f m\r\n\r\n",
                odometro.persistencia_disponivel ? "ativa" : "indisponível",
                odometro.persistencia_pendente ? " (pendente)" : "",
                static_cast<unsigned long>(odometro.erros_persistencia),
                configuracao::INTERVALO_PERSISTENCIA_ODOMETRO_M);

    std::printf("DIAGNÓSTICO DOS SENSORES\r\n");
    std::printf("  ADC              : erros=%lu | último=%s\r\n",
                static_cast<unsigned long>(sensores.adc.total),
                esp_err_to_name(sensores.adc.ultimo_erro));
    std::printf("  MPU6050          : erros=%lu | último=%s\r\n",
                static_cast<unsigned long>(sensores.mpu6050.total),
                esp_err_to_name(sensores.mpu6050.ultimo_erro));
    std::printf("  Encoder          : erros=%lu | último=%s\r\n",
                static_cast<unsigned long>(sensores.encoder.total),
                esp_err_to_name(sensores.encoder.ultimo_erro));
    std::printf("  INA228           : erros=%lu | último=%s\r\n",
                static_cast<unsigned long>(sensores.ina228.total),
                esp_err_to_name(sensores.ina228.ultimo_erro));
    std::printf("  Barramento I2C   : operações=%lu | erros=%lu | último=%s\r\n\r\n",
                static_cast<unsigned long>(i2c.operacoes_ok),
                static_cast<unsigned long>(i2c.erros),
                esp_err_to_name(i2c.ultimo_erro));

    std::printf("  [1] Atualizar leituras e diagnóstico\r\n");
    std::printf("  [2] Zerar somente o percurso da sessão\r\n");
    std::printf("  [3] Solicitar relatório completo nos logs\r\n");
    std::printf("  [4] Ver somente o leitor de tensão (ADC)\r\n");
    std::printf("  [5] Ver somente o MPU6050\r\n");
    std::printf("  [6] Ver somente encoders e odômetro\r\n");
    std::printf("  [7] Ver somente o barramento I2C\r\n");
    std::printf("  [8] Ver somente o INA228\r\n");
    std::printf("  [9] Pausar ou retomar leituras dos sensores\r\n");
    imprimir_prompt("[Enter] Voltar ao menu principal");
}

void mostrar_controle_sensores() {
    imprimir_titulo("CONTROLE DE LEITURAS | DIAGNÓSTICO");
    std::printf("  Escolha um sensor para alternar seu estado:\r\n");
    constexpr SensorControlado lista[] = {
        SensorControlado::Adc, SensorControlado::Mpu6050,
        SensorControlado::Encoders, SensorControlado::Ina228,
        SensorControlado::TemperaturaEsp};
    constexpr const char* nomes[] = {"ADC", "MPU6050", "Encoders", "INA228", "Temperatura ESP"};
    for (size_t indice = 0; indice < 5; ++indice) {
        std::printf("  [%u] %-10s : %s\r\n", static_cast<unsigned>(indice + 3),
                    nomes[indice],
                    servico_aquisicao_sensor_ativo(lista[indice]) ? "leitura permitida" : "PAUSADO");
    }
    std::printf("\r\n  [1] Pausar todas as leituras\r\n");
    std::printf("  [2] Retomar todas as leituras\r\n");
    std::printf("\r\nA escolha vale até reiniciar o mestre. Na simulação, os dados continuam\r\n");
    std::printf("simulados. O PCNT continua contando enquanto a leitura dos encoders está\r\n");
    std::printf("pausada; a velocidade publicada vira zero, sem indicar que o carro parou.\r\n");
    std::printf("O MPU6050 refaz a calibração ao voltar.\r\n");
    imprimir_prompt("[Enter] Voltar a sensores e diagnóstico");
}

void mostrar_leitura_adc() {
    const RetratoDiagnosticoSistema retrato =
        servico_diagnostico_obter_retrato();
    imprimir_titulo("LEITOR DE TENSÃO | ADC");
    if (!servico_aquisicao_sensor_ativo(SensorControlado::Adc)) {
        std::printf("Aquisição pausada pelo terminal. O valor anterior não é mostrado.\r\n");
        imprimir_rodape_monitor_sensor();
        return;
    }
    std::printf("GPIO              : %d\r\n", configuracao::PINO_ACELERADOR_ADC);
    std::printf("Leitura bruta     : %u de 4095 (%.1f%% da escala)\r\n",
                static_cast<unsigned>(retrato.telemetria_atual.tensao_adc_bruta),
                retrato.telemetria_atual.tensao_adc_bruta * 100.0f / 4095.0f);
    std::printf("Erros             : %lu\r\n",
                static_cast<unsigned long>(retrato.sensores.adc.total));
    std::printf("Último resultado  : %s\r\n",
                esp_err_to_name(retrato.sensores.adc.ultimo_erro));
    std::printf("\r\nA conversão para volts depende do divisor resistivo e da calibração do ADC.\r\n");
    imprimir_rodape_monitor_sensor();
}

void mostrar_leitura_ina228() {
    const EstadoIna228 sensor = ina228_obter_estado();
    const LeituraIna228& leitura = sensor.ultima_leitura;
    imprimir_titulo("INA228 | MEDIÇÃO ELÉTRICA");
    if (servico_simulador_telemetria_esta_ativo())
        std::printf("Simulador ativo: o INA228 não recebe novas leituras; dados abaixo são históricos.\r\n");
    if (!servico_aquisicao_sensor_ativo(SensorControlado::Ina228)) {
        std::printf("Aquisição pausada pelo terminal. O valor anterior não é mostrado.\r\n");
        imprimir_rodape_monitor_sensor();
        return;
    }
    std::printf("Estado            : %s | último=%s\r\n",
                sensor.inicializado ? "ativo" : "indisponível",
                esp_err_to_name(sensor.ultimo_erro));
    std::printf("Endereço / ID     : 0x%02X / 0x%04X | calibração=%u\r\n",
                configuracao::ENDERECO_INA228, sensor.identificacao,
                sensor.calibracao);
    std::printf("Shunt / limite    : %.3f mΩ / %.1f A\r\n",
                configuracao::RESISTENCIA_SHUNT_INA228_OHM * 1000.0f,
                configuracao::CORRENTE_MAXIMA_INA228_A);
    std::printf("Leituras / erros  : %lu / %lu (consecutivos=%lu)\r\n",
                static_cast<unsigned long>(sensor.leituras_ok),
                static_cast<unsigned long>(sensor.erros_leitura),
                static_cast<unsigned long>(sensor.erros_consecutivos));
    if (leitura.valida) {
        std::printf("Idade da leitura  : %llu ms\r\n",
                    static_cast<unsigned long long>(
                        esp_timer_get_time() / 1000 - leitura.instante_ms));
        std::printf("VBUS / VSHUNT     : %.3f V / %+.4f mV\r\n",
                    leitura.tensao_barramento_v, leitura.tensao_shunt_mv);
        std::printf("Corrente / potência: %+.3f A / %+.1f W\r\n",
                    leitura.corrente_a, leitura.potencia_w);
        std::printf("Potência interna  : %.1f W (magnitude sem sinal)\r\n",
                    leitura.potencia_chip_w);
        std::printf("Temperatura chip  : %.2f °C\r\n", leitura.temperatura_chip_c);
        if (leitura.energia_valida)
            std::printf("Energia acumulada : %.2f Wh (desde reset do INA)\r\n",
                        leitura.energia_j / 3600.0);
        else
            std::printf("Energia acumulada : indisponível (transbordamento)\r\n");
        if (leitura.carga_valida)
            std::printf("Carga acumulada   : %+.3f Ah (desde reset do INA)\r\n",
                        leitura.carga_c / 3600.0);
        else
            std::printf("Carga acumulada   : indisponível (transbordamento)\r\n");
        std::printf("Diagnóstico bruto : 0x%04X\r\n", leitura.diagnostico);
    } else {
        std::printf("Leitura           : indisponível; zero não é medição\r\n");
    }
    imprimir_rodape_monitor_sensor();
}

void mostrar_leitura_mpu6050() {
    const EstadoMpu6050 sensor = mpu6050_obter_estado();
    const EstadoServicoAceleracao aceleracao =
        servico_aceleracao_obter_estado();
    const LeituraMpu6050& leitura = aceleracao.leitura;
    imprimir_titulo("MPU6050 | LEITURA DETALHADA");
    if (!servico_aquisicao_sensor_ativo(SensorControlado::Mpu6050)) {
        std::printf("Aquisição pausada pelo terminal. Ao retomar, a calibração será refeita.\r\n");
        imprimir_rodape_monitor_sensor();
        return;
    }
    std::printf("Estado            : %s\r\n",
                sensor.inicializado ? "ativo" : "INATIVO");
    std::printf("WHO_AM_I          : 0x%02X (%s)\r\n",
                static_cast<unsigned>(sensor.identificacao),
                sensor.identificacao == 0x68 ? "padrão" : "não padrão, aceito");
    std::printf("Endereço I2C      : 0x%02X\r\n",
                static_cast<unsigned>(configuracao::ENDERECO_MPU6050));
    std::printf("Configuração      : %s | ±2 g | ±250 °/s\r\n",
                 sensor.configuracao_verificada ? "verificada" : "NÃO VERIFICADA");
    std::printf("Serviço aquisição : %s\r\n",
                aceleracao.iniciado ? "ativo" : "INATIVO");
    std::printf("Amostragem / filtros: %u Hz | DLPF: %u Hz | longitudinal: %.1f Hz\r\n",
                static_cast<unsigned>(sensor.frequencia_amostragem_hz),
                static_cast<unsigned>(sensor.frequencia_filtro_acelerometro_hz),
                configuracao::FREQUENCIA_CORTE_ACELERACAO_HZ);
    std::printf("Acelerômetro bruto: X=%d Y=%d Z=%d\r\n",
                leitura.aceleracao_x, leitura.aceleracao_y,
                leitura.aceleracao_z);
    std::printf("Aceleração em g   : X=%+.4f Y=%+.4f Z=%+.4f\r\n",
                leitura.aceleracao_x_g, leitura.aceleracao_y_g,
                leitura.aceleracao_z_g);
    std::printf("Aceleração m/s²   : X=%+.3f Y=%+.3f Z=%+.3f\r\n",
                leitura.aceleracao_x_ms2, leitura.aceleracao_y_ms2,
                leitura.aceleracao_z_ms2);
    std::printf("Gravidade estimada: X=%+.4f Y=%+.4f Z=%+.4f g\r\n",
                aceleracao.gravidade_estimada_x_g,
                aceleracao.gravidade_estimada_y_g,
                aceleracao.gravidade_estimada_z_g);
    std::printf("Aceleração linear : X=%+.4f Y=%+.4f Z=%+.4f g\r\n",
                aceleracao.aceleracao_linear_x_g,
                aceleracao.aceleracao_linear_y_g,
                aceleracao.aceleracao_linear_z_g);
    std::printf("Giroscópio bruto  : X=%d Y=%d Z=%d\r\n",
                leitura.giroscopio_x, leitura.giroscopio_y,
                leitura.giroscopio_z);
    std::printf("Giroscópio °/s    : X=%+.2f Y=%+.2f Z=%+.2f\r\n",
                leitura.giroscopio_x_graus_s,
                leitura.giroscopio_y_graus_s,
                leitura.giroscopio_z_graus_s);
    std::printf("Desvio giroscópio : X=%+.3f Y=%+.3f Z=%+.3f °/s\r\n",
                aceleracao.desvio_giroscopio_x_graus_s,
                aceleracao.desvio_giroscopio_y_graus_s,
                aceleracao.desvio_giroscopio_z_graus_s);
    std::printf("Temperatura       : %.2f °C\r\n", leitura.temperatura_c);
    std::printf("Longitudinal      : %+.4f g | %+.3f m/s²\r\n",
                aceleracao.aceleracao_longitudinal_g,
                aceleracao.aceleracao_longitudinal_ms2);
    const char* sentido = aceleracao.sentido ==
                                  SentidoAceleracaoLongitudinal::Frente
                              ? "para frente"
                          : aceleracao.sentido ==
                                  SentidoAceleracaoLongitudinal::Tras
                              ? "para trás"
                              : "neutro";
    const char eixo = configuracao::EIXO_LONGITUDINAL_MPU6050 == 0
                          ? 'X'
                      : configuracao::EIXO_LONGITUDINAL_MPU6050 == 1 ? 'Y'
                                                                     : 'Z';
    std::printf("Sentido aceleração: %s | eixo %c %s\r\n", sentido, eixo,
                aceleracao.eixo_longitudinal_compativel
                    ? "compatível"
                    : "INCOMPATÍVEL COM A MONTAGEM");
    std::printf("Calibração        : %s | grav. longitudinal=%+.5f g | %u/%u amostras\r\n",
                aceleracao.calibrado ? "concluída" : "aguardando repouso",
                aceleracao.componente_gravidade_longitudinal_g,
                static_cast<unsigned>(
                    aceleracao.amostras_calibracao_coletadas),
                static_cast<unsigned>(
                    configuracao::AMOSTRAS_CALIBRACAO_MPU6050));
    std::printf("Leituras/erros    : dispositivo=%lu/%lu | serviço=%lu/%lu\r\n",
                static_cast<unsigned long>(sensor.leituras_ok),
                static_cast<unsigned long>(sensor.erros_leitura),
                static_cast<unsigned long>(aceleracao.leituras_ok),
                static_cast<unsigned long>(aceleracao.erros_leitura));
    std::printf("Último resultado  : dispositivo=%s | serviço=%s\r\n",
                esp_err_to_name(sensor.ultimo_erro),
                esp_err_to_name(aceleracao.ultimo_erro));
    imprimir_rodape_monitor_sensor();
}

void mostrar_leitura_encoders() {
    const RetratoDiagnosticoSistema retrato =
        servico_diagnostico_obter_retrato();
    const auto& esquerdo = retrato.encoder_esquerdo;
    const auto& direito = retrato.encoder_direito;
    const auto& odometro = retrato.odometro;
    imprimir_titulo("ENCODERS E ODÔMETRO | LEITURA DETALHADA");
    if (!servico_aquisicao_sensor_ativo(SensorControlado::Encoders)) {
        std::printf("Leitura pausada pelo terminal; PCNT continua contando os pulsos.\r\n");
        imprimir_rodape_monitor_sensor();
        return;
    }
    std::printf("Esquerdo          : %s | contagem=%llu | giros=%.3f\r\n",
                esquerdo.inicializado ? "ativo" : "INATIVO",
                static_cast<unsigned long long>(esquerdo.contagem_atual),
                esquerdo.contagens_por_giro == 0 ? 0.0
                    : static_cast<double>(esquerdo.contagem_atual) /
                          esquerdo.contagens_por_giro);
    std::printf("  overflow/erros  : %lu/%lu | percurso=%.3f m | %.2f km/h\r\n",
                static_cast<unsigned long>(
                    esquerdo.transbordamentos_compensados),
                static_cast<unsigned long>(esquerdo.erros_leitura),
                odometro.roda_esquerda.percurso_m,
                odometro.roda_esquerda.velocidade_kmh);
    std::printf("Direito           : %s | contagem=%llu | giros=%.3f\r\n",
                direito.inicializado ? "ativo" : "INATIVO",
                static_cast<unsigned long long>(direito.contagem_atual),
                direito.contagens_por_giro == 0 ? 0.0
                    : static_cast<double>(direito.contagem_atual) /
                          direito.contagens_por_giro);
    std::printf("  overflow/erros  : %lu/%lu | percurso=%.3f m | %.2f km/h\r\n",
                static_cast<unsigned long>(
                    direito.transbordamentos_compensados),
                static_cast<unsigned long>(direito.erros_leitura),
                odometro.roda_direita.percurso_m,
                odometro.roda_direita.velocidade_kmh);
    std::printf("Média das rodas   : %.3f m | %.2f km/h\r\n",
                odometro.percurso_m, odometro.velocidade_kmh);
    std::printf("Odômetro total    : %.3f m | rodas %s\r\n",
                odometro.odometro_m,
                odometro.rodas_coerentes ? "coerentes" : "DIVERGENTES");
    imprimir_rodape_monitor_sensor();
}

void mostrar_leitura_i2c() {
    const EstatisticasI2c i2c = gerenciador_i2c_obter_estatisticas();
    imprimir_titulo("BARRAMENTO I2C | DIAGNÓSTICO");
    std::printf("Estado            : %s\r\n",
                i2c.inicializado ? "ativo" : "INATIVO");
    std::printf("Porta / SDA / SCL : %d / GPIO%d / GPIO%d\r\n",
                configuracao::PORTA_I2C, configuracao::PINO_I2C_SDA,
                configuracao::PINO_I2C_SCL);
    std::printf("Frequência        : %lu Hz\r\n",
                static_cast<unsigned long>(configuracao::FREQUENCIA_I2C_HZ));
    std::printf("Operações/erros   : %lu/%lu\r\n",
                static_cast<unsigned long>(i2c.operacoes_ok),
                static_cast<unsigned long>(i2c.erros));
    std::printf("Bytes TX/RX       : %llu/%llu\r\n",
                static_cast<unsigned long long>(i2c.bytes_escritos),
                static_cast<unsigned long long>(i2c.bytes_lidos));
    std::printf("Sondagens OK/aus. : %lu/%lu\r\n",
                static_cast<unsigned long>(i2c.sondagens_ok),
                static_cast<unsigned long>(i2c.enderecos_ausentes));
    std::printf("Último resultado  : %s\r\n",
                esp_err_to_name(i2c.ultimo_erro));
    imprimir_rodape_monitor_sensor();
}

void tratar_menu_sensores(unsigned long opcao) {
    switch (opcao) {
        case 1:
            break;
        case 2: {
            const esp_err_t erro = servico_odometro_zerar_percurso();
            if (erro == ESP_OK) {
                std::printf("\r\n[OK] Percurso da sessão zerado; o odômetro total foi preservado.\r\n");
            } else {
                std::printf("\r\n[ERRO] Não foi possível zerar o percurso: %s.\r\n",
                            esp_err_to_name(erro));
            }
            break;
        }
        case 3:
            solicitar_resumo();
            break;
        case 4:
            tela_atual = TelaSensores::Adc;
            return;
        case 5:
            tela_atual = TelaSensores::Mpu6050;
            return;
        case 6:
            tela_atual = TelaSensores::Encoders;
            return;
        case 7:
            tela_atual = TelaSensores::I2c;
            return;
        case 8:
            tela_atual = TelaSensores::Ina228;
            return;
        case 9:
            tela_atual = TelaSensores::Controle;
            mostrar_controle_sensores();
            return;
        default:
            std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
            break;
    }
    mostrar_menu_sensores();
}

void tratar_controle_sensores(unsigned long opcao) {
    if (opcao == 1 || opcao == 2) {
        servico_aquisicao_definir_todos_sensores(opcao == 2);
        ESP_LOGI(ETIQUETA, "Aquisição de todos os sensores %s pelo terminal",
                 opcao == 2 ? "retomada" : "pausada");
    } else if (opcao >= 3 && opcao <= 7) {
        const SensorControlado sensor =
            static_cast<SensorControlado>(opcao - 3);
        const bool ativar = !servico_aquisicao_sensor_ativo(sensor);
        servico_aquisicao_definir_sensor(sensor, ativar);
        ESP_LOGI(ETIQUETA, "Aquisição do sensor %lu %s pelo terminal",
                 opcao - 3, ativar ? "retomada" : "pausada");
    } else {
        std::printf("\r\nOpção inválida.\r\n");
    }
    mostrar_controle_sensores();
}

}  // namespace

void abrir() {
    tela_atual = TelaSensores::Principal;
    mostrar_menu_sensores();
}

bool voltar() {
    if (tela_atual == TelaSensores::Principal) return false;
    abrir();
    return true;
}

bool tratar_opcao(unsigned long opcao) {
    switch (tela_atual) {
        case TelaSensores::Principal:
            tratar_menu_sensores(opcao);
            return true;
        case TelaSensores::Controle:
            tratar_controle_sensores(opcao);
            return true;
        default:
            return false;
    }
}

bool monitorando() {
    return tela_atual == TelaSensores::Adc ||
           tela_atual == TelaSensores::Mpu6050 ||
           tela_atual == TelaSensores::Encoders ||
           tela_atual == TelaSensores::I2c ||
           tela_atual == TelaSensores::Ina228;
}

void atualizar_monitor() {
    // Monitores seriais simples não interpretam controle de cursor ANSI; cada
    // atualização é um novo bloco, em intervalo deliberadamente lento.
    std::printf("\r\n");
    switch (tela_atual) {
        case TelaSensores::Adc:
            mostrar_leitura_adc();
            break;
        case TelaSensores::Mpu6050:
            mostrar_leitura_mpu6050();
            break;
        case TelaSensores::Encoders:
            mostrar_leitura_encoders();
            break;
        case TelaSensores::I2c:
            mostrar_leitura_i2c();
            break;
        case TelaSensores::Ina228:
            mostrar_leitura_ina228();
            break;
        default:
            break;
    }
    std::fflush(stdout);
}

}  // namespace terminal_sensores
