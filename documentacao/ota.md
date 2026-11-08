# Atualização OTA do mestre e dos satélites

[Índice da documentação](README.md)

O mestre consulta periodicamente `manifesto-firmwares.json` no último GitHub
Release. O arquivo descreve separadamente os firmwares `mestre-s3`, `equipe-s3` e
`visitantes-s3`, cada um com sua própria versão, tamanho, URL e SHA-256.

O Wi-Fi permanece desligado fora das verificações e instalações. Quando há uma
atualização, o operador escolhe no terminal qual alvo autorizar. O mestre atualiza
sua própria partição por HTTPS. Para um satélite, ele baixa a imagem por HTTPS e a
retransmite pela UART em blocos confirmados individualmente.

Uma atualização encontrada aguarda autorização por até 5 min. Se ninguém
autorizar nesse prazo, o mestre descarta a oferta e retorna ao estado de
cancelamento, sem alterar o firmware em execução. Uma nova consulta ao manifesto
poderá encontrá-la outra vez. O cabeçalho HTTPS `Date` da consulta também permite
acertar o relógio UTC dos logs no SD quando ele ainda não tem hora válida.

Na operação **Atualizar todos**, a ordem é equipe, visitantes e, por último,
mestre. Um alvo sem atualização pendente é ignorado. Se um satélite falhar, o
mestre encerra a sequência e preserva seu próprio firmware para que o operador
possa consultar o diagnóstico e repetir ou executar cada atualização
individualmente.

O protocolo OTA dos satélites usa sessão, sequência, deslocamento e CRC-32. Blocos
sem confirmação são repetidos sem regravação duplicada. O satélite grava somente a
partição inativa, compara o SHA-256 completo, seleciona a nova imagem e reinicia.
A sessão identifica todos os quadros: um `CANCELAR` de sessão antiga não aborta
uma transferência nova. O prazo de uma transferência parcial é verificado pelo
receptor mesmo quando continua chegando tráfego de outra família pela UART.
A imagem nova fica pendente enquanto Wi-Fi, UART e servidor web inicializam e por
mais 2 min de observação. Se reiniciar antes da confirmação, o bootloader retorna
à versão anterior. Depois da confirmação, o firmware ainda observa reinícios por
panic ou watchdog até completar 5 min; três eventos desse tipo solicitam
rollback. A contagem fica na memória RTC e é perdida em um corte total de energia.
O mesmo mecanismo protege o mestre. O mestre considera a transferência de um
satélite concluída quando a nova versão responde; a confirmação interna da
imagem pelo satélite ocorre após a observação de 2 min.

No mestre, o download dos firmwares dos satélites usa três buffers fixos de 4 KiB
na PSRAM. Enquanto uma tarefa transmite e confirma um bloco pela UART, o cliente
HTTPS pode preencher o seguinte. Os buffers são criados uma vez ao iniciar o
serviço e reutilizados, evitando fragmentação e mantendo o quadro entregue ao
driver UART na RAM interna. Se a PSRAM não puder ser usada, há fallback para RAM
interna.

Durante a transferência, o terminal e a SSD1306 mostram o mesmo estado mantido
pelo serviço OTA: alvo, versão, bytes confirmados, porcentagem, taxa em MB/s e tempo
restante. A última faixa da OLED é usada como barra de progresso.

Atualização disponível, conclusão e falha possuem indicação sonora própria. Um
aviso pendente não monopoliza a OLED: ele aparece inicialmente e volta em
intervalos regulares, permitindo visualizar as páginas normais entre os lembretes.

## Primeira gravação por cabo

O mestre N16R8 e os dois ESP32-S3 SuperMini precisam receber uma gravação inicial
por cabo. Essa instalação grava as tabelas com duas partições OTA. Nos SuperMini de
4 MB, cada partição de aplicação possui 1.920 KiB.

```powershell
pio run -d firmware\mestre-s3 -e mestre-s3-n16r8 -t upload
pio run -d firmware\equipe-s3 -e esp32-s3-devkitm-1 -t upload
pio run -d firmware\visitantes-s3 -e esp32-s3-devkitm-1 -t upload
```

Depois dessa preparação, os satélites continuam usando os GPIO 5 (RX) e 6 (TX) dos
SuperMini. O GND deve ser comum ao mestre.

## Menu do mestre

No terminal de 115200 baud, escolha `2 - Atualização OTA`. O submenu permite:

```text
1 - Atualizar o estado exibido
2 - Verificar nova versão agora
3 - Autorizar atualização do mestre
4 - Autorizar atualização do satélite da equipe
5 - Autorizar atualização do satélite de visitantes
6 - Atualizar todos: satélites e depois mestre
7 - Cancelar atualizações pendentes
8 - Alterar intervalo das verificações
9 - Alterar intervalo da telemetria durante OTA
```

O mestre só oferece OTA ao satélite que esteja respondendo, suporte OTA e tenha
uma versão menor que a descrita no manifesto.

As mesmas autorizações estão disponíveis na página **Comandos** de
`http://192.168.4.1/`, quando o operador está conectado ao ponto de acesso do
módulo da equipe. A página exige
confirmação para ações que alteram estado e acompanha o resultado pela UART.
O envio HTTP também exige a chave administrativa do ESP da Equipe, diferente
da senha Wi-Fi; veja [satélites](satelites.md).

## Preparar um release

O processo completo de escolha entre **major, minor e patch**, versões
independentes por ESP, conferência dos binários, tag e anexos está em
[versionamento](versionamento.md). As opções e saídas de cada script estão no
[manual de ferramentas](ferramentas.md). O caminho normal é:

```powershell
python ferramentas\preparar_release_ota.py --bump patch
```

O preparador detecta fontes alteradas, incrementa somente as versões internas
afetadas e a etiqueta do conjunto, regenera os painéis web, compila, confere a versão de cada imagem e
gera `versoes/saida/manifesto-firmwares.json`. A publicação precisa conter esse
manifesto e os três `.bin` da mesma preparação. O repositório precisa ser
público enquanto o download HTTPS do firmware não tiver autenticação para
releases privados. O fluxo
`.github/workflows/publicar-firmware-mestre.yml` executa testes, verifica que
os painéis gerados correspondem às fontes versionadas e pode recompilar e
publicar os quatro anexos quando uma tag compatível com o estado versionado é
enviada. O manifesto OTA atual usa `versao_formato` 2 e reúne os três alvos;
o exemplo antigo de manifesto exclusivo do mestre não deve ser publicado.
