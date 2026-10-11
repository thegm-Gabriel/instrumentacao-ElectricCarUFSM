# Arquitetura do ESP32-S3 mestre

O código ativo está dividido por responsabilidade:

- `nucleo`: configuração de placa, Wi-Fi e OTA.
- `gerenciadores`: acesso genérico aos periféricos do ESP-IDF. Não conhecem o formato da telemetria.
- `dispositivos`: regras específicas de cada componente físico, como MPU6050 e entrada ADC.
- `servicos`: funcionalidades da aplicação, como telemetria, diagnóstico, Wi-Fi,
  OTA, sinalização e menu do terminal serial.
- `main.cpp`: inicialização e coordenação periódica. Não acessa periféricos diretamente.

As dependências devem seguir somente essa direção: `nucleo` e protocolos
compartilhados → gerenciadores → dispositivos → serviços → apresentação e
`main.cpp`; cada camada à direita só pode depender das camadas à esquerda.
Cabeçalhos públicos de serviços não devem agregar tipos concretos de
outros serviços ou gerenciadores. O retrato de diagnóstico, por isso, publica
tipos neutros próprios e é preenchido apenas na implementação.
Dependências entre serviços devem ser unidirecionais; quando dois serviços
precisarem colaborar sem uma hierarquia natural, `main.cpp` registra um callback
ou compõe a operação, evitando referências recíprocas.

Inicializações que possuem mais de uma etapa são transacionais: em caso de
falha, tarefas, filas, mutexes, registros de supervisão e drivers criados pela
operação são liberados em ordem inversa. Chamadas de inicialização concluídas
são idempotentes. Funções assíncronas usam nomes que deixam o contrato claro;
por exemplo, `gerenciador_uart_enfileirar` confirma a cópia para a fila, enquanto
ACK, timeout e repetição pertencem aos protocolos de telemetria, comandos e OTA.

Os gerenciadores preservam o código de erro do ESP-IDF. Os serviços decidem
tentativa, degradação e recuperação, e `main.cpp` decide se a falha impede a
execução. Resultados esperados, como endereço I2C sem ACK durante uma sondagem,
não entram no mesmo contador de falhas do barramento. O diagnóstico registra
totais, falhas consecutivas e instantes da primeira e da última ocorrência dos
sensores, sem alocação dinâmica.

## Estado atual

UART, I2C, PCNT, cartão SD e OTA possuem implementação funcional. I2S não faz
parte do componente ativo e só deve ser adicionado quando houver um caso de uso.

O OTA usa Wi-Fi em modo estação, HTTPS com o pacote de certificados do ESP-IDF,
manifesto único no GitHub Release, SHA-256, duas partições de aplicação e rollback.
A orquestração e a autorização ficam em `servicos/servico_ota.cpp`. A gravação do
mestre fica no gerenciador OTA; o download e o transporte confirmado para os
satélites ficam em `transporte_ota_satelites`. `servico_enlace_satelites` é o único
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

`servicos/servico_diagnostico.cpp` reúne as estatísticas dos módulos, publica uma
classificação de saúde atualizada periodicamente e oferece um agregado único para
as interfaces de observabilidade. `servicos/servico_relatorios.cpp` é responsável pela
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
v4 identifica versão, tipo e tamanho da carga, transporta as grandezas processadas
do MPU6050 e protege o quadro com CRC-16. Assim,
o ciclo de aquisição não aguarda a conclusão física de cada transmissão e os
receptores conseguem distinguir corrupção, incompatibilidade e conteúdo inválido.
Os satélites devolvem uma confirmação curta para cada quadro válido. O serviço de
telemetria mede, separadamente, RTT atual, médio e máximo e a idade da última
resposta dos módulos da equipe e dos visitantes.

Na OTA dos satélites, HTTPS e UART formam um pipeline produtor–consumidor. Três
buffers de 4 KiB são alocados uma única vez, preferencialmente na PSRAM, e
reutilizados durante toda a execução. O callback HTTP somente preenche e enfileira
blocos; uma tarefa persistente monta o quadro em RAM interna, transmite e aguarda
a confirmação. Assim, a rede pode preencher o próximo bloco enquanto a UART
confirma o anterior, sem alocação por bloco e sem usar PSRAM como memória DMA.
Quando a PSRAM não está disponível, a inicialização tenta a RAM interna e informa
a degradação no registro.

O buffer de recepção do manifesto também é reservado uma única vez na
inicialização do serviço OTA e reutilizado nas verificações periódicas. Dessa
forma, apenas as estruturas transitórias do analisador JSON permanecem dinâmicas.

A apresentação ao vivo usa um retrato a 4 Hz. Em paralelo,
`servico_registro_telemetria` reduz esse mesmo retrato para 1 Hz e o entrega por
fila a uma única tarefa proprietária dos CSVs. O arquivo ativo é sincronizado a
cada dez segundos, fechado a cada cinco minutos e recuperado após reinicialização.
O upload é iniciado explicitamente no fim da sessão, abre uma sessão Wi-Fi
temporária, exige confirmação nominal do Google e só então move o CSV para
`enviados`. O gerenciador do cartão conhece apenas SPI e montagem FAT; toda a
política permanece no serviço.

Os gerenciadores retornam `esp_err_t`. O código chamador deve sempre verificar o
resultado antes de usar os dados ou avançar para a próxima etapa.

`gerenciador_contador_pulsos` mantém duas instâncias genéricas do periférico PCNT
e conta as quatro transições de cada par A/B. Os dispositivos
`encoder_incremental` representam os encoders físicos esquerdo e direito e
conhecem pinos, P/R e contagens por giro. `servico_odometro` aplica a geometria e
a relação mecânica de cada roda, usa a média das duas distâncias para calcular
velocidade, percurso e distância total e persiste o odômetro periodicamente na
NVS. O percurso é reiniciado com o firmware; a distância total é recuperada após
reinicializações. O serviço também limita saltos incompatíveis com a velocidade
máxima configurada e diagnostica divergência persistente entre as rodas, sem
alterar a média usada pelo odômetro.

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

O painel também mantém uma fila de mensagens de tamanho fixo. A inicialização
publica etapa e percentual sem espera artificial; avisos são copiados para a fila
e exibidos depois da partida. Estados persistentes usam janelas recorrentes, de
forma que um alerta continua sendo lembrado sem monopolizar permanentemente a
tela. Nenhuma mensagem faz alocação dinâmica durante a operação.

`servicos/servico_supervisao.cpp` acompanha pulsos da tarefa de telemetria e dos
dois canais UART. Cada alvo possui tempo limite compatível com o maior intervalo
configurável e recuperação independente. A supervisão mede a margem de pilha das
tarefas associadas. Os canais UART são recuperados restaurando filas, buffers,
pinos e velocidade sem reiniciar o restante do mestre; a tarefa de telemetria não
é apagada à força enquanto pode estar usando um periférico.
