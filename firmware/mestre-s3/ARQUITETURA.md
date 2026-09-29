# Arquitetura do ESP32-S3 mestre

O código ativo está dividido por responsabilidade:

- `nucleo`: configuração de placa, Wi-Fi e OTA.
- `gerenciadores`: acesso genérico aos periféricos do ESP-IDF. Não conhecem o formato da telemetria.
- `dispositivos`: regras específicas de cada componente físico, como MPU6050 e entrada ADC.
- `servicos`: funcionalidades da aplicação, como telemetria, diagnóstico, Wi-Fi,
  OTA, sinalização e menu do terminal serial.
- `main.cpp`: inicialização e coordenação periódica. Não acessa periféricos diretamente.

## Estado atual

UART, I2C e OTA possuem implementação funcional. I2S e cartão SD possuem contratos,
validação de argumentos, estados e códigos de erro, mas ainda retornam
`ESP_ERR_NOT_SUPPORTED`. Eles devem ser completados depois que forem definidos:

- I2S: direção, formato, DMA e pinos de clock/dados.
- SD: SPI ou SDMMC, pinagem, ponto de montagem e política de gravação.

O OTA usa Wi-Fi em modo estação, HTTPS com o pacote de certificados do ESP-IDF,
manifesto único no GitHub Release, SHA-256, duas partições de aplicação e rollback.
A orquestração e a autorização ficam em `servicos/servico_ota.cpp`. A gravação do
mestre fica no gerenciador OTA; o download e o transporte confirmado para os
satélites ficam em `gerenciador_ota_satelites`. `servico_enlace_satelites` é o único
proprietário da inicialização e da supervisão das UARTs dos satélites. Ele consome
os quadros de retorno e encaminha cada tipo ao serviço correto. O
`servico_telemetria` apenas monta e publica os dados e acompanha as confirmações;
ele não controla tarefas nem drivers de comunicação.

As conexões Wi-Fi são sessões temporárias. O gerenciador mede RSSI, canal e motivo
de desconexão, enquanto o serviço decide a prioridade dos perfis, as autorizações
e quando o rádio deve entrar em repouso.

`servicos/servico_wifi.cpp` mantém a política de redes: prioridade da rede principal,
perfis secundários persistidos na NVS, autorização manual e modo automático
protegido por senha. `gerenciadores/gerenciador_wifi.cpp` cuida apenas da conexão e
da troca de credenciais no ESP-IDF. Assim, terminal e telemetria podem usar a mesma
API sem duplicar regras.

`servicos/servico_diagnostico.cpp` reúne as estatísticas dos módulos e publica uma
classificação de saúde atualizada periodicamente. Seus getters apenas devolvem o
último retrato calculado. `servicos/servico_relatorios.cpp` é responsável pela
formatação, periodicidade e pausa dos relatórios e consulta diagnóstico, comandos
e demais serviços sem criar dependências de retorno entre eles. O terminal apenas
apresenta e edita essas opções. O `main.cpp` inicializa a
NVS global, inicia os módulos e coordena a aquisição periódica. O modelo
`compartilhado/telemetria/dados_telemetria_veiculo.h` impede que o serviço de
telemetria dependa diretamente do MPU6050 ou do simulador.

Os comandos remotos executam consultas e ações pelos contratos públicos dos
serviços. Para ações de interface, como solicitar um resumo, o `main.cpp`
registra a função correspondente no serviço de comandos. Isso mantém comandos e
relatórios sem dependência circular.

`servicos/servico_simulador_telemetria.cpp` é uma fonte substituível de dados de
teste. Ele simula movimento, bateria, posição e sensores de proximidade, mas não
conhece UART nem páginas web. O `main.cpp` escolhe essa fonte por configuração e
entrega o mesmo modelo ao `servico_telemetria` usado pelos sensores reais.

A transmissão UART usa uma fila independente por destino. O protocolo compartilhado
v3 identifica versão, tipo e tamanho da carga e protege o quadro com CRC-16. Assim,
o ciclo de aquisição não aguarda a conclusão física de cada transmissão e os
receptores conseguem distinguir corrupção, incompatibilidade e conteúdo inválido.
Os satélites devolvem uma confirmação curta para cada quadro válido. O serviço de
telemetria mede, separadamente, RTT atual, médio e máximo e a idade da última
resposta dos módulos da equipe e dos visitantes.

Os gerenciadores retornam `esp_err_t`. O código chamador deve sempre verificar o
resultado antes de usar os dados ou avançar para a próxima etapa.

`gerenciadores/gerenciador_indicadores.cpp` contém apenas o acesso aos quatro LEDs
e ao PWM do buzzer passivo. `servicos/servico_sinalizacao.cpp` interpreta os
estados de saúde, Wi-Fi e OTA, escolhe o padrão visual e agenda melodias sem
bloquear as outras tarefas. A avaliação dos serviços ocorre a cada 500 ms; a
máquina de LEDs e notas avança a cada 50 ms.

`dispositivos/tela_ssd1306.cpp` implementa o controlador OLED, a fonte e o
framebuffer usando exclusivamente a API do `gerenciador_i2c`. O
`servicos/servico_painel_local.cpp` consulta o estado já classificado pelo serviço
de sinalização e apresenta inicialização, saúde, Wi-Fi e OTA. A tela compartilha o
barramento do MPU6050 e só recebe um novo quadro quando o conteúdo realmente muda.
Uma falha do painel é isolada e não interrompe a telemetria.

`servicos/servico_supervisao.cpp` acompanha pulsos da tarefa de telemetria e dos
dois canais UART. Cada alvo possui tempo limite compatível com o maior intervalo
configurável e recuperação independente. A supervisão mede a margem de pilha das
tarefas associadas. Os canais UART são recuperados restaurando filas, buffers,
pinos e velocidade sem reiniciar o restante do mestre; a tarefa de telemetria não
é apagada à força enquanto pode estar usando um periférico.
