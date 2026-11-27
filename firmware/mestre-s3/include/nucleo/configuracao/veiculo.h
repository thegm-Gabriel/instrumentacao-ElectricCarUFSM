// Calibração, mecânica, controle e simulação.
#pragma once

#include <cstdint>

namespace configuracao {

// Estado de carga por tensão em repouso. O banco nominal de 48 V tem 4 baterias.
constexpr uint8_t SOC_BATERIAS_SERIE       = 4;
constexpr uint16_t SOC_VAZIO_POR_BATERIA_MV = 11200;
constexpr uint16_t SOC_CHEIO_POR_BATERIA_MV = 13000;
constexpr uint32_t SOC_REPOUSO_MS          = 1000;

// Pedal e habilitação de saídas
// Faixa útil lida pelo ADC, margem de falha e travas de atuação.
constexpr uint16_t PEDAL_TENSAO_SOLTO_MV       = 500;
constexpr uint16_t PEDAL_TENSAO_PRESSIONADO_MV = 2500;
constexpr uint16_t PEDAL_LIMIAR_SOLTO_MV       = 650;
constexpr uint16_t PEDAL_MARGEM_FALHA_MV       = 200;
// Tensão inicial do DAC acima da zona morta. Com pedal solto, a saída é 0 mV.
constexpr uint16_t DAC_TENSAO_MINIMA_SAIDA_MV = 1000;
// A saída analógica só pode atuar após validar circuito, intertravamento e
// tensão em bancada. O DAC é inicializado e zerado mesmo com esta trava.
constexpr bool HABILITAR_SAIDA_ACELERADOR_DAC = true;
// Relé no contato NA: nível baixo na bobina deixa o driver desabilitado.
// As saídas seguem a pinagem da placa e os intertravamentos do controle.
constexpr bool HABILITAR_SAIDAS_VEICULO = true;
// Perfil de medição do pedal e do DAC com VOUT desconectado da tração.
constexpr bool HABILITAR_DAC_ISOLADO = HABILITAR_SAIDA_ACELERADOR_DAC && !HABILITAR_SAIDAS_VEICULO;

// Permitir ré inicialmente ainda deixa a direção solicitada em frente.
constexpr bool PERMITIR_REVERSAO_PADRAO = true;
constexpr uint16_t ZONA_MORTA_PEDAL_PADRAO_DECIMOS = 50;
constexpr uint16_t ACELERADOR_MAXIMO_PADRAO_DECIMOS = 1000;
constexpr uint32_t VELOCIDADE_MAXIMA_PADRAO_CENTESIMOS = 3000;
constexpr uint8_t MARCHA_MAXIMA_PADRAO = 3; // H
constexpr bool PARADA_EMERGENCIA_NA_PARTIDA = false;

// Controle do veículo
// Validade das leituras, confirmação da marcha e rampa do acelerador.
constexpr uint32_t PRAZO_ACELERACAO_MS                 = 300;
constexpr uint32_t PRAZO_VELOCIDADE_MS                 = 150;
constexpr uint32_t PEDAL_SOLTO_PARA_ARMAR_MS           = 500;
constexpr uint32_t VALIDADE_SOLICITACAO_REARME_MS      = 2000;
constexpr uint32_t ESTABILIZACAO_CHAVE_MARCHA_MS       = 60;
constexpr uint32_t VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS = 50; // 0,5 km/h
constexpr uint16_t SUBIDA_ACELERADOR_DECIMOS_POR_S     = 250; // 25 pontos percentuais/s
constexpr uint32_t INTERVALO_ATUALIZACAO_DAC_MS        = 20;  // 50 Hz com saída habilitada
constexpr uint32_t INTERVALO_DAC_SEM_TRACAO_MS         = 1000;
constexpr uint32_t INTERVALO_RESONDAGEM_DAC_MS         = 5000;

// Teto de tração progressivo perto do limite. Valores iniciais para ensaio.
constexpr uint32_t FAIXA_LIMITADOR_CENTESIMOS_KMH      = 300;
constexpr uint32_t MARGEM_RETORNO_CENTESIMOS_KMH       = 30;
constexpr uint8_t AMOSTRAS_RETORNO_LIMITADOR           = 2;

// A telemetria consulta estas amostras com a cadência própria do enlace.
constexpr uint32_t PERIODO_ODOMETRO_MS                 = 20;   // 50 Hz
constexpr uint32_t PERIODO_SENSORES_ELETRICOS_MS        = 100;  // 10 Hz
constexpr uint32_t PRAZO_SENSORES_ELETRICOS_MS          = 300;

// MPU6050
// Eixo longitudinal, taxa de amostragem e filtros da aceleração.
// Montagem atual: X vertical (+ para cima) e Y longitudinal (+ para frente).
// Use 0/1/2 para X/Y/Z e troque o sinal para -1,0 se a orientação mudar.
constexpr uint8_t EIXO_LONGITUDINAL_MPU6050            = 1;
constexpr float SINAL_EIXO_LONGITUDINAL_MPU6050        = 1.0f;
constexpr uint32_t FREQUENCIA_AMOSTRAGEM_MPU6050_HZ    = 10;
constexpr float FREQUENCIA_CORTE_ACELERACAO_HZ         = 3.0f;
constexpr uint16_t AMOSTRAS_CALIBRACAO_MPU6050         = 10;  // Cerca de 1 s
constexpr float LIMITE_GIRO_CALIBRACAO_MPU6050_GRAUS_S = 5.0f;
constexpr float GRAVIDADE_MINIMA_CALIBRACAO_MPU6050_G  = 0.85f;
constexpr float GRAVIDADE_MAXIMA_CALIBRACAO_MPU6050_G  = 1.15f;
// Impede calibrar como longitudinal um eixo praticamente vertical. Ajuste o
// eixo X/Y/Z acima conforme a montagem se esta validação rejeitar a calibração.
constexpr float GRAVIDADE_MAXIMA_EIXO_LONGITUDINAL_G = 0.50f;
// O giroscópio acompanha rapidamente a orientação; o acelerômetro corrige
// lentamente a deriva apenas quando a aceleração linear estimada está baixa.
constexpr float FREQUENCIA_CORRECAO_GRAVIDADE_HZ       = 0.05f;
constexpr float LIMITE_ACELERACAO_CORRECAO_GRAVIDADE_G = 0.012f;
// Em repouso confirmado pelo odômetro, acompanhe o desvio térmico dos sensores.
constexpr uint32_t TEMPO_CONFIRMACAO_REPOUSO_MPU6050_MS = 2000;
constexpr float LIMITE_GIRO_REPOUSO_MPU6050_GRAUS_S     = 1.0f;
constexpr float LIMITE_VARIACAO_REPOUSO_MPU6050_G       = 0.025f;
constexpr float FREQUENCIA_CORRECAO_REPOUSO_MPU6050_HZ  = 0.5f;
// Histerese e confirmação evitam inversões rápidas causadas por vibração.
constexpr float LIMIAR_ENTRADA_SENTIDO_ACELERACAO_G        = 0.03f;
constexpr float LIMIAR_SAIDA_SENTIDO_ACELERACAO_G          = 0.015f;
constexpr uint32_t TEMPO_CONFIRMACAO_SENTIDO_ACELERACAO_MS = 80;

// Recuperação localizada após falha de comunicação com o MPU6050.
constexpr uint8_t  FALHAS_SEGUIDAS_PARA_RECUPERAR_MPU = 3;
constexpr uint32_t INTERVALO_RECUPERACAO_MPU_MS      = 5000;

// Encoders e odômetro
// Habilite os lados instalados. Uma roda sem encoder contribui com zero na média.
constexpr bool USAR_ENCODER_ESQUERDO = true;
constexpr bool USAR_ENCODER_DIREITO  = false;
static_assert(USAR_ENCODER_ESQUERDO || USAR_ENCODER_DIREITO);

// Resolução das entradas e parâmetros usados para calcular a distância.
constexpr uint32_t PULSOS_ENCODER_ESQUERDO_POR_GIRO = 600;
constexpr uint32_t PULSOS_ENCODER_DIREITO_POR_GIRO  = 600;
constexpr uint32_t FILTRO_RUIDO_ENCODER_NS          = 1000;

// Ajuste cada lado conforme a montagem mecânica. A relação representa quantos
// giros o encoder completa para cada giro da roda (redução 4:1 = 4,0).
constexpr float DIAMETRO_RODA_ESQUERDA_M             = 1.2f;
constexpr float DIAMETRO_RODA_DIREITA_M              = 1.2f;
constexpr float GIROS_ENCODER_ESQUERDO_POR_GIRO_RODA = 5.0f;
constexpr float GIROS_ENCODER_DIREITO_POR_GIRO_RODA  = 5.0f;

// Distância entre gravações do total na NVS. Aumentar reduz o desgaste da flash
// e aumenta a distância que pode ser perdida em uma queda súbita de energia.
constexpr float INTERVALO_PERSISTENCIA_ODOMETRO_M = 100.0f;

// Limites usados apenas para diagnosticar leituras fisicamente improváveis.
// A distância continua sendo a média das duas rodas.
constexpr float VELOCIDADE_MAXIMA_ODOMETRO_KMH       = 200.0f;
constexpr float RAZAO_MINIMA_MOVIMENTO_ENTRE_RODAS   = 0.20f;
constexpr float MOVIMENTO_MINIMO_DIAGNOSTICO_RODAS_M = 0.05f;
constexpr uint32_t JANELA_DIAGNOSTICO_RODAS_MS        = 250;
constexpr uint16_t AMOSTRAS_DIVERGENTES_PARA_ALERTA  = 10;

constexpr int64_t INTERVALO_REPETICAO_PERSISTENCIA_US = 30000000;

// Simulação
// Fonte inicial da telemetria, dinâmica e descarga da bateria simulada.
constexpr bool USAR_TELEMETRIA_SIMULADA          = false;
constexpr uint16_t DINAMICA_SIMULACAO_PERCENTUAL = 100;

constexpr uint16_t DINAMICA_MINIMA        = 25;
constexpr uint16_t DINAMICA_MAXIMA        = 200;
constexpr int64_t PASSO_CARGA_SIMULADA_US = 250000;
constexpr float QUEDA_CARGA_POR_PASSO     = 0.1f;

constexpr float CARGA_INICIAL_SIMULADA_PERCENTUAL = 88.0f;

}  // namespace configuracao
