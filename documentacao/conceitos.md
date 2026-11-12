# Conceitos, siglas e periféricos

[Índice da documentação](README.md) · [Arquitetura](arquitetura.md) · [Pinagem](pinagem.md)

Esta página explica os termos usados nos guias e o motivo de cada recurso do
ESP32-S3 aparecer no projeto. Para pinos, níveis elétricos e limites concretos,
use a [pinagem](pinagem.md); para o formato dos pacotes, use [protocolos](protocolos.md).

## Comunicação e verificação

| Termo | Significado neste projeto |
|---|---|
| **UART** (*Universal Asynchronous Receiver/Transmitter*) | Comunicação serial entre o mestre e cada satélite. Cada enlace tem TX e RX, permitindo telemetria em um sentido e confirmações, comandos e respostas no outro. |
| **Baud / 8N1** | Baud indica a taxa de símbolos da porta serial; aqui, 1.000.000 baud. `8N1` significa oito bits de dados, sem bit de paridade e um bit de parada. As duas pontas devem usar a mesma configuração. |
| **ACK** (*acknowledgment*, confirmação) | Resposta de recebimento. O ACK da telemetria devolve sequência e tempo do mestre; ele confirma o quadro recebido, não que um comando demorado terminou. |
| **CRC** (*cyclic redundancy check*) | Valor calculado sobre os bytes de um quadro para detectar corrupção na transmissão. Telemetria e ACK usam CRC-16; comandos e OTA de satélites usam CRC-32. CRC não autentica a origem. |
| **Timeout / prazo esgotado** | Tempo máximo de espera por resposta ou avanço. Ao vencer, a operação informa falha ou tenta recuperar conforme seu contrato; não se presume sucesso. |
| **RTT** (*round-trip time*) | Tempo entre enviar um quadro e receber seu retorno. Ajuda a avaliar atraso do enlace UART sem sincronizar os relógios dos ESPs. |
| **HTTP / HTTPS** (*Hypertext Transfer Protocol* / HTTP seguro) | Protocolo usado pelo navegador para consultar os painéis. HTTPS acrescenta TLS, usado pelo mestre nas conexões externas de OTA e Google. O painel local usa HTTP na rede Wi-Fi do satélite. |
| **TLS** (*Transport Layer Security*) | Protege a conexão HTTPS e permite verificar o servidor por certificado. É diferente de CRC ou SHA-256: estes detectam alteração de dados, mas não estabelecem sozinhos uma conexão autenticada. |
| **OTA** (*over-the-air*) | Atualização do firmware sem cabo. O mestre baixa imagens por HTTPS e, para os satélites, transfere blocos confirmados por UART. O bootloader pode reverter uma imagem que não seja validada após a partida. |
| **SHA-256** (*Secure Hash Algorithm*, 256 bits) | Função de hash que produz um resumo da imagem. O tamanho e o SHA-256 do arquivo baixado são comparados com o manifesto antes da instalação; isso detecta arquivo incompleto ou diferente. Não substitui a validação da origem do manifesto. |
| **RSSI** (*received signal strength indicator*) | Intensidade aproximada do sinal Wi-Fi recebido, normalmente em dBm. Valores mais próximos de zero indicam sinal mais forte; não medem, sozinhos, a qualidade completa da conexão. |
| **SPI / I²C / I²S** | Barramentos distintos: SPI2 liga o mestre ao microSD; I²C compartilha dois fios entre MPU6050, INA228 e OLED; I²S recebe áudio digital do microfone na função temporária de treinamento. |
| **MPU6050 / INA228 / OLED** | MPU6050 mede aceleração e rotação; INA228 mede tensão e corrente por meio de um shunt e acumula energia/carga; OLED é a tela local. Compartilham o I²C com endereços distintos. |
| **P/R** | Pulsos por revolução do encoder. Os modelos documentados têm 600 P/R; o firmware conta as quatro bordas dos canais A e B, obtendo 2.400 contagens por giro. Isso não implica, nesta configuração, leitura do sentido. |
| **CRC-16/CCITT-FALSE** | Variante de CRC-16 fixada pelo protocolo de telemetria. Os três firmwares precisam usar a mesma variante e a mesma ordem dos bytes. |

## Software, memória e arquivos

| Termo | Significado neste projeto |
|---|---|
| **ESP-IDF** (*Espressif IoT Development Framework*) | Biblioteca e ambiente do fabricante usados pelos três firmwares; oferece drivers, rede, OTA, armazenamento e APIs de tarefas. |
| **Firmware / bootloader / rollback** | Firmware é o programa gravado no ESP. O bootloader escolhe a partição a iniciar; rollback é o retorno à imagem anterior quando a nova não é confirmada conforme as regras de OTA. |
| **API / endpoint** | API é o conjunto de operações oferecidas pelo servidor web; endpoint é uma rota específica, como `/api/telemetria`. |
| **Driver / gerenciador** | O driver é a interface de baixo nível com um periférico. O `gerenciador_*` do projeto encapsula esse driver e entrega uma API estável aos serviços, sem decidir o significado da leitura para o carro. |
| **Callback** | Função registrada para ser chamada quando ocorre um evento ou quando um módulo precisa devolver um resultado. Aqui também permite ligar serviços na inicialização sem criar referências circulares entre eles. |
| **Parser / checksum / pipeline** | Parser separa e interpreta quadros recebidos; checksum é um valor de conferência usado, por exemplo, na cópia de configuração; pipeline é o fluxo em que o download prepara um bloco enquanto outro é enviado pela UART. |
| **`esp_err_t`** | Tipo de retorno de várias funções do ESP-IDF. `ESP_OK` indica sucesso; outros valores identificam a falha. Preservar o código original ajuda a diagnosticar se o problema ocorreu no driver, no barramento ou na operação pedida. |
| **FreeRTOS / tarefa / fila** | O sistema de tarefas usado pelo ESP-IDF. Tarefas executam atividades com ritmos diferentes; filas passam dados ou pedidos sem obrigar a aquisição a esperar por rede ou cartão. |
| **CPU / heap / pilha** | CPU executa as tarefas; heap é a área para alocações dinâmicas; cada tarefa tem sua própria pilha para chamadas e variáveis locais. Memória livre e margem de pilha ajudam a investigar falhas. |
| **Watchdog** | Supervisor que detecta uma tarefa ou sistema sem progresso dentro do prazo esperado. Um disparo pode causar reinício e integra a observação após OTA. |
| **DMA** (*direct memory access*) | Transferência feita pelo periférico com menos intervenção da CPU. Buffers DMA precisam estar em região de memória compatível; PSRAM não é substituta automática da RAM interna nesses casos. |
| **PSRAM** | RAM externa, volátil. Guarda histórico recente de diagnóstico e buffers reutilizados quando disponível; seus dados desaparecem ao desligar. |
| **NVS** (*non-volatile storage*) | Área persistente na flash para preferências e valores pequenos, como odômetro e redes salvas. Não é o local dos arquivos CSV e logs. |
| **RTC / UTC** | RTC aqui designa a memória retida em certos reinícios, usada pela guarda de rollback; ela não sobrevive necessariamente a um corte total de energia. UTC é o horário de referência registrado nos logs quando uma data confiável foi obtida por HTTPS. |
| **CSV / JSON** | Formatos de texto: CSV guarda amostras tabulares da telemetria; JSON descreve o manifesto OTA e respostas de algumas APIs. |
| **FAT / KiB / MiB** | FAT é o sistema de arquivos do microSD. KiB e MiB são unidades binárias: 1 KiB = 1.024 bytes; 1 MiB = 1.024 KiB. |
| **AP / DHCP / DNS** | AP é o ponto de acesso Wi-Fi criado por um satélite. DHCP entrega endereço IP ao cliente; DNS resolve nomes. O portal de visitantes usa DNS cativo quando o serviço está disponível. |
| **GPIO** (*general-purpose input/output*) | Pino configurável do ESP32-S3 para entrada, saída ou função de periférico. Um GPIO de 3,3 V não deve receber diretamente 12/24 V. |
| **ADC** (*analog-to-digital converter*) | Conversor de tensão analógica em contagem digital. A leitura atual do mestre é bruta; sem calibração e circuito de escala, não representa diretamente a tensão da bateria. |
| **PCNT** (*pulse counter*) | Contador de pulsos por hardware usado nos dois encoders. Conta bordas sem a CPU consultar continuamente os pinos. |
| **PWM / LEDC** | PWM alterna uma saída em frequência e tempo ligado controlados. O periférico LEDC gera esse sinal para o buzzer passivo, permitindo tons sem temporização manual da CPU. |
| **NPN / pull-up** | NPN é o tipo de transistor sugerido para acionar o buzzer sem exigir sua corrente do GPIO. Pull-up é o resistor que leva um sinal aberto a 3,3 V; as saídas dos encoders precisam dele. |
| **Amostra / conjunto de leituras** | Valores publicados juntos em um instante, com origem e validade. Uma amostra antiga continua visível no painel, mas sua idade deve ser considerada antes de interpretá-la como estado atual. |

## Por que usar os periféricos do ESP32-S3

| Recurso em uso | Aplicação e vantagem | Limite importante |
|---|---|---|
| **PCNT** | Uma unidade por encoder conta as bordas de A e B; o odômetro lê o acumulado para calcular percurso e velocidade. O filtro de pulsos curtos e a compensação de limite ajudam a manter contagens estáveis e longas sem polling da CPU. | A configuração atual incrementa nas duas bordas de cada canal, sem usar a relação de fase entre A e B: **não identifica sentido de rotação**. Avanço e recuo somam percurso. |
| **UART** | Duas portas independentes separam Equipe e Visitantes. O hardware serializa os bytes e o software usa filas, validação e confirmações; a página web não precisa compartilhar o tempo da aquisição. | TX, RX, GND, nível de 3,3 V e configuração serial devem coincidir nas duas pontas. |
| **I²C** | MPU6050, INA228 e OLED usam o mesmo par SDA/SCL com endereços diferentes, economizando GPIOs. O driver concentra acesso e contabiliza falhas. | Um dispositivo ausente ou barramento preso pode atrasar uma transação; a falha é tratada sem atribuir zero a uma medição inválida. |
| **SPI2 e DMA** | O mestre acessa o microSD para CSV, logs e cópia de configuração. O barramento e buffers compatíveis permitem transferências eficientes enquanto a aquisição segue em outra tarefa. | Cartão e sistema de arquivos podem atrasar ou falhar; por isso as amostras chegam ao escritor por fila. |
| **ADC em leitura pontual** | Lê a entrada analógica apenas quando solicitada pelo ciclo de aquisição; é simples para a medição bruta atual. | A contagem de 0 a 4095 exige condicionamento e calibração antes de virar tensão física confiável. |
| **I²S** | Recebe o fluxo digital do INMP441 sem amostrar cada bit em software; é usado apenas na gravação temporária de áudio para treinamento. | Não participa da telemetria contínua e disputa recursos com gravação no SD. |
| **LEDC** | Produz PWM para o buzzer passivo e troca tons sem laços de espera que prendam a CPU. | O buzzer deve ter acionamento elétrico adequado à corrente exigida. |
| **Wi-Fi** | Os satélites criam redes locais para os painéis. O mestre abre sessões temporárias de estação para consulta OTA e envio ao Google. | Disponibilidade de rádio e internet não deve bloquear a aquisição ou o enlace UART. |

`PCNT`, `UART`, `I²C`, `SPI2`, `ADC`, `I²S` e `LEDC` são periféricos ou
controladores do chip. **PSRAM e NVS são recursos de memória**, e FreeRTOS é o
sistema de tarefas; a distinção importa ao escolher onde guardar dados e
quem deve executar cada operação.
