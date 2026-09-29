# Firmware do ESP32-S3 mestre

Alvo de hardware: **ESP32-S3-WROOM-1-N16R8**, com 16 MB de flash Quad SPI e
8 MB de PSRAM Octal SPI.

Adquire tensão e MPU6050 ou gera uma telemetria simulada, monta o pacote e o envia
aos módulos da equipe e de visitantes por UART. O modo é escolhido por
`USAR_TELEMETRIA_SIMULADA` em `configuracao_placa.h`; enquanto estiver habilitado,
o terminal identifica claramente os dados como simulados. Também verifica
atualizações no GitHub e só executa
o OTA após autorização pelo terminal. Consulte `ARQUITETURA.md` para a divisão
interna e `../../documentacao/ota/README.md` para preparar os releases.

## Redes Wi-Fi pelo terminal

No menu serial, a opção `5` permite testar uma rede informada pelo usuário, decidir
se ela deve ser salva, conectar a redes já salvas e excluí-las. A rede principal é
sempre tentada primeiro na inicialização. Redes secundárias só são tentadas após
confirmação, salvo quando o modo automático tiver sido habilitado por uma operação
protegida.

A troca da rede principal, sua restauração e a alteração do modo automático exigem
`WIFI_SENHA_PERMISSAO` em `include/nucleo/segredos_ota.h`. Se a constante não for
definida, o firmware usa `OTA_WIFI_SENHA` por compatibilidade. As credenciais e as
preferências ficam na NVS; nenhuma senha aparece nos relatórios ou nos logs.

Canal preferencial, largura e potência do rádio são configurados em
`include/nucleo/configuracao_wifi.h` pelas flags `WIFI_CANAL_PREFERENCIAL`,
`WIFI_LARGURA_CANAL_MHZ` e `WIFI_POTENCIA_MAXIMA_DBM`. Como o mestre trabalha
em modo estação, o canal é apenas uma indicação de onde iniciar a busca; o
roteador determina o canal usado após a associação. O valor `0` aceita qualquer
canal.

As funções públicas de `include/servicos/servico_wifi.h` não dependem do menu. Elas
podem ser chamadas futuramente pelo serviço de telemetria mantendo as mesmas
validações e regras de persistência.

## LEDs e buzzer passivo

O mestre possui um serviço não bloqueante de sinalização para inicialização,
saúde, Wi-Fi, OTA, avisos, falhas e confirmações do terminal. Os quatro LEDs usam
os GPIOs 10 a 13 e o buzzer passivo usa PWM no GPIO 14. As ligações e o circuito
de acionamento recomendado estão em `../../hardware/pinagem/README.md`.

O acesso elétrico fica em `gerenciador_indicadores`; a interpretação dos estados
e as melodias ficam em `servico_sinalizacao`. Assim, os demais serviços apenas
publicam seu estado ou um evento e não precisam conhecer GPIO, PWM ou notas.

| Indicação | Significado |
|---|---|
| Azul e amarelo em animação | Inicialização |
| Verde contínuo | Sistema saudável |
| Verde com pulso azul duplo | Sessão Wi-Fi ativa |
| Azul e amarelo alternados | Verificação OTA |
| Azul contínuo com pulso amarelo duplo | Atualização aguardando autorização |
| Azul com animação verde e amarela | Firmware OTA sendo baixado |
| Sequência verde, azul e amarela | Atualização concluída |
| Amarelo duplo | Atenção na saúde do sistema |
| Amarelo contínuo com vermelho duplo | Falha OTA; alarme a cada 15 segundos |
| Vermelho em SOS | Falha crítica; alarme a cada 8 segundos |

## Tela local SSD1306

Uma tela SSD1306 I²C de 0,96 polegada e 128×64 pixels pode mostrar o estado do
mestre sem depender do terminal. Ela apresenta inicialização, saúde dos satélites,
sessão Wi-Fi, etapas da atualização OTA, avisos e falhas críticas. O painel usa os
mesmos GPIOs 8 e 9 do MPU6050 e detecta automaticamente os endereços `0x3C` e
`0x3D`.

O framebuffer fica em `dispositivos/tela_ssd1306.cpp`. A seleção das mensagens e
a atualização periódica ficam em `servicos/servico_painel_local.cpp`, sem misturar
o protocolo elétrico da tela com as regras de estado da aplicação.
