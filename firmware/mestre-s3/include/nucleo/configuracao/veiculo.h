// Calibração, mecânica, controle e simulação.
#pragma once

#include <cstdint>

namespace configuracao {

// Pedal e habilitação de saídas
// Faixa útil lida pelo ADC, margem de falha e travas de atuação.
constexpr uint16_t PEDAL_TENSAO_SOLTO_MV       = 500;
constexpr uint16_t PEDAL_TENSAO_PRESSIONADO_MV = 2500;
constexpr uint16_t PEDAL_MARGEM_FALHA_MV       = 200;
// Tensão inicial do DAC acima da zona morta. Com pedal solto, a saída é 0 mV.
constexpr uint16_t DAC_TENSAO_MINIMA_SAIDA_MV = 0;
// A saída analógica só pode atuar após validar circuito, intertravamento e
// tensão em bancada. O DAC é inicializado e zerado mesmo com esta trava.
constexpr bool HABILITAR_SAIDA_ACELERADOR_DAC = false;
// Relé no contato NA: nível baixo na bobina deixa o driver desabilitado.
// A atuação fica bloqueada até atribuir pinos livres e validar o circuito.
constexpr bool HABILITAR_SAIDAS_VEICULO = false;

// Controle do veículo
// Validade das leituras, confirmação da marcha e rampa do acelerador.
constexpr uint32_t PRAZO_ACELERACAO_MS                 = 300;
constexpr uint32_t PRAZO_VELOCIDADE_MS                 = 1000;
constexpr uint32_t PEDAL_SOLTO_PARA_ARMAR_MS           = 500;
constexpr uint32_t ESTABILIZACAO_CHAVE_MARCHA_MS       = 60;
constexpr uint32_t VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS = 50; // 0,5 km/h
constexpr uint16_t PASSO_ACELERADOR_DECIMOS            = 5; // até 25%/s a cada 20 ms
constexpr uint32_t INTERVALO_RESONDAGEM_DAC_MS         = 5000;

// MPU6050
// Eixo longitudinal, taxa de amostragem e filtros da aceleração.
// Montagem atual: X vertical (+ para cima) e Y longitudinal (+ para frente).
// Use 0/1/2 para X/Y/Z e troque o sinal para -1,0 se a orientação mudar.
constexpr uint8_t EIXO_LONGITUDINAL_MPU6050            = 1;
constexpr float SINAL_EIXO_LONGITUDINAL_MPU6050        = 1.0f;
constexpr uint32_t FREQUENCIA_AMOSTRAGEM_MPU6050_HZ    = 100;
constexpr float FREQUENCIA_CORTE_ACELERACAO_HZ         = 3.0f;
constexpr uint16_t AMOSTRAS_CALIBRACAO_MPU6050         = 100;
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
// Histerese e confirmação evitam inversões rápidas causadas por vibração.
constexpr float LIMIAR_ENTRADA_SENTIDO_ACELERACAO_G        = 0.03f;
constexpr float LIMIAR_SAIDA_SENTIDO_ACELERACAO_G          = 0.015f;
constexpr uint32_t TEMPO_CONFIRMACAO_SENTIDO_ACELERACAO_MS = 80;

// Recuperação localizada após falha de comunicação com o MPU6050.
constexpr uint8_t  FALHAS_SEGUIDAS_PARA_RECUPERAR_MPU = 3;
constexpr uint32_t INTERVALO_RECUPERACAO_MPU_MS      = 5000;

// Encoders e odômetro
// Resolução das entradas e parâmetros usados para calcular a distância.
constexpr uint32_t PULSOS_ENCODER_ESQUERDO_POR_GIRO = 600;
constexpr uint32_t PULSOS_ENCODER_DIREITO_POR_GIRO  = 600;
constexpr uint32_t FILTRO_RUIDO_ENCODER_NS          = 1000;

// Ajuste cada lado conforme a montagem mecânica. A relação representa quantos
// giros o encoder completa para cada giro da roda (redução 4:1 = 4,0).
constexpr float DIAMETRO_RODA_ESQUERDA_M             = 0.300f;
constexpr float DIAMETRO_RODA_DIREITA_M              = 0.300f;
constexpr float GIROS_ENCODER_ESQUERDO_POR_GIRO_RODA = 1.0f;
constexpr float GIROS_ENCODER_DIREITO_POR_GIRO_RODA  = 1.0f;

// Distância entre gravações do total na NVS. Aumentar reduz o desgaste da flash
// e aumenta a distância que pode ser perdida em uma queda súbita de energia.
constexpr float INTERVALO_PERSISTENCIA_ODOMETRO_M = 100.0f;

// Limites usados apenas para diagnosticar leituras fisicamente improváveis.
// A distância continua sendo a média das duas rodas.
constexpr float VELOCIDADE_MAXIMA_ODOMETRO_KMH       = 200.0f;
constexpr float RAZAO_MINIMA_MOVIMENTO_ENTRE_RODAS   = 0.20f;
constexpr float MOVIMENTO_MINIMO_DIAGNOSTICO_RODAS_M = 0.05f;
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
