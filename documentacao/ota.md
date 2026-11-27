# Versionamento, releases e atualização OTA

[Índice da documentação](README.md)

O mestre consulta periodicamente `manifesto-firmwares.json` no último GitHub
Release. O arquivo descreve separadamente os firmwares `mestre-s3`, `equipe-s3` e
`visitantes-s3`, cada um com sua própria versão, tamanho, URL e SHA-256.

O Mestre usa o Wi-Fi externo em sessões temporárias de OTA, Google e teste de
rede. Quando há uma atualização, o operador escolhe o alvo no terminal ou no
painel da Equipe. O Mestre baixa sua própria imagem por HTTPS para a partição
inativa. Para um satélite, retransmite a imagem pela UART em blocos confirmados
individualmente.

Uma atualização encontrada aguarda autorização por até 5 min. Se ninguém
autorizar nesse prazo, o mestre descarta a oferta e retorna ao estado de
cancelamento, sem alterar o firmware em execução. Uma nova consulta ao manifesto
poderá encontrá-la outra vez. O cabeçalho HTTPS `Date` da consulta também permite
acertar o relógio UTC dos logs no SD quando ele ainda não tem hora válida.

## Autorização, Wi-Fi e cancelamento

1. A consulta ao manifesto cria uma oferta. A tela **Aguardando autorização**
   indica que o download da imagem ainda não começou.
2. Após autorizar, o serviço tenta obter a sessão Wi-Fi. Uma sessão concorrente
   de Google ou um teste de rede pode impedir a reserva. Nesse caso, a oferta
   volta a **Aguardando autorização** até o prazo original. Aguarde a liberação
   do Wi-Fi e autorize novamente ainda dentro do prazo. Se o prazo acabar, faça
   outra verificação. Uma falha de conexão sem disputa aparece como **Falha**.
3. Com a sessão Wi-Fi obtida e a autorização ainda válida, o Mestre desarma o
   controle do veículo antes de iniciar qualquer download ou gravação de imagem.
   Com a saída do MCP4725 habilitada, a tarefa de controle precisa confirmar
   uma nova escrita de 0 mV no DAC dentro do prazo. Se a preparação falhar,
   a instalação é interrompida. Quando a
   operação termina com o mesmo firmware ainda em execução no Mestre, o
   operador precisa rearmar manualmente depois de um desarme por OTA.

**Cancelar** descarta a oferta pendente. Durante a preparação da rede ou a
instalação, registra a solicitação para a tarefa de OTA interromper a operação.
A resposta ao comando confirma o recebimento do pedido. Confira o estado
**Cancelado** para confirmar o resultado. A tarefa pode precisar esperar a
chamada de rede em curso terminar ou expirar.

O serviço aceita pedidos até o início da finalização de cada alvo. No Mestre,
isso acontece depois de conferir o tamanho e o SHA-256. Nos satélites, acontece
antes do envio de **Finalizar** pela UART. Durante a finalização e o retorno do
alvo, aguarde o resultado. No Mestre, **Aplicado** indica que a partição foi
selecionada e será usada no próximo boot. No lote, o cancelamento fica
disponível de novo durante a transferência do próximo alvo.

Se o download do Mestre for interrompido, o gerenciador tenta restabelecer a
mesma rede e continua na partição inativa a partir de um setor já gravado.
A retomada exige o intervalo exato da imagem na resposta HTTPS e a validação
final do SHA-256 do manifesto. O serviço pode recomeçar do zero quando o
servidor não oferece o intervalo esperado. As tentativas são limitadas e
pertencem à mesma instalação autorizada.

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
O mesmo mecanismo protege o Mestre. Quando o ACK final se perde, o Mestre
consulta pela UART se a versão esperada voltou a responder. **Em observação**
indica que ela já está em execução e o boot ainda está pendente. **Concluído**
indica que o satélite confirmou a imagem. Para firmware que só informa a
versão, o painel usa **Versão em execução**. O lote pode prosseguir assim que
o satélite voltar com a versão esperada.

No mestre, o download dos firmwares dos satélites usa três buffers fixos de 4 KiB
na PSRAM. Enquanto uma tarefa transmite e confirma um bloco pela UART, o cliente
HTTPS pode preencher o seguinte. Os buffers são criados uma vez ao iniciar o
serviço e reutilizados junto com o quadro UART, também alocado na PSRAM. Se a
PSRAM estiver indisponível, o serviço tenta usar RAM interna. A leitura UART
entrega os bytes disponíveis assim que chegam.
O cliente HTTP fecha a transferência quando detecta cancelamento ou falha.
Se a tarefa de envio ainda estiver encerrando a sessão, uma nova tentativa
fica disponível quando ela devolver os buffers.

No Mestre, os buffers de dados HTTPS e o buffer de escrita da imagem usam a
PSRAM quando disponível. A recepção Wi-Fi reserva seis buffers fixos de cerca
de 1,6 KiB cada, ou mais se a janela de recepção do SDK exigir. O log `Heap
interno OTA` registra o espaço livre, o maior bloco e o mínimo desde a partida
antes da conexão, com a rede conectada e ao final de cada transferência.

Durante a transferência, o terminal e a SSD1306 mostram o mesmo estado mantido
pelo serviço OTA: alvo, versão, bytes confirmados, porcentagem, taxa em MB/s e tempo
restante. A última faixa da OLED é usada como barra de progresso.

Atualização disponível, conclusão e falha possuem indicação sonora própria. Um
aviso pendente não monopoliza a OLED: ele aparece inicialmente e volta em
intervalos regulares, permitindo visualizar as páginas normais entre os lembretes.

## Primeira gravação por cabo

O Mestre N16R8 e os dois ESP32-C3 SuperMini Plus precisam receber a primeira gravação por cabo. No C3, use o USB-C nativo. A instalação grava o bootloader e a tabela de partições do chip. Cada slot de aplicação do C3 tem 1.920 KiB. A troca de ESP32-S3 para ESP32-C3 exige essa gravação inicial, pois a OTA atualiza somente uma aplicação do mesmo chip.

```powershell
pio run -d firmware\mestre-s3 -e mestre-s3-n16r8 -t upload
pio run -d firmware\equipe-s3 -e esp32-c3-supermini-plus -t upload
pio run -d firmware\visitantes-s3 -e esp32-c3-supermini-plus -t upload
```

Depois dessa preparação, os satélites continuam usando os GPIO 5 (RX) e 6 (TX) dos
SuperMini Plus. O GND deve ser comum ao mestre. As pastas `equipe-s3` e `visitantes-s3` e as chaves correspondentes do manifesto foram mantidas para compatibilidade com as ferramentas e o Mestre. Os binários de release `equipe-c3.bin` e `visitantes-c3.bin` identificam o chip de destino.

## Menu do mestre

No terminal de 115200 baud, escolha `2 - Atualização OTA`. O submenu permite:

| Opção | Ação |
|---:|---|
| 1 | Atualizar o estado exibido. |
| 2 | Verificar nova versão agora. |
| 3 | Autorizar atualização do mestre. |
| 4 | Autorizar atualização do satélite da Equipe. |
| 5 | Autorizar atualização do satélite de Visitantes. |
| 6 | Atualizar todos: satélites e depois mestre. |
| 7 | Cancelar a oferta ou a instalação enquanto o serviço aceitar o cancelamento. |
| 8 | Alterar o intervalo das verificações. |
| 9 | Alterar o intervalo da telemetria durante OTA. |

O mestre só oferece OTA ao satélite que esteja respondendo, suporte OTA e tenha
uma versão menor que a descrita no manifesto.

As mesmas autorizações estão disponíveis na página **Comandos** de
`http://192.168.4.1/`, quando o operador está conectado ao ponto de acesso do
módulo da equipe. A página exige
confirmação para ações que alteram estado e acompanha o resultado pela UART.
O envio HTTP fica disponível aos clientes conectados ao AP da Equipe; veja
[satélites](satelites.md).

A Equipe consulta o progresso com `GET /api/mestre?consulta=ota`. As ações
usam `POST /api/mestre?consulta=...` com `verificar_ota`, `autorizar_mestre`,
`autorizar_equipe`, `autorizar_visitantes`, `autorizar_todos` e `cancelar_ota`.

## Escolher a versão

O projeto usa `MAJOR.MINOR.PATCH` tanto na etiqueta do release quanto na versão
interna de cada firmware. A etiqueta identifica o **conjunto** publicado; mestre,
Equipe e Visitantes têm versões próprias. Um release pode, por exemplo, conter
um mestre sem alteração e um satélite novo. O preparador incrementa a etiqueta
e apenas os alvos cujas fontes mudaram.

| Incremento | Use quando | Exemplo a partir de `2.4.6` |
|---|---|---|
| **Major** | Uma mudança deliberadamente incompatível exige migração, atualização coordenada ou nova instalação; por exemplo, trocar o formato UART sem suporte ao anterior. Zera minor e patch. | `3.0.0` |
| **Minor** | Uma funcionalidade nova preserva a compatibilidade esperada; por exemplo, adicionar um campo versionado que os firmwares antigos conseguem ignorar. Zera patch. | `2.5.0` |
| **Patch** | Uma correção ou refinamento mantém o contrato; por exemplo, ajustar uma falha, o desempenho ou o painel web. | `2.4.7` |

`major` não significa apenas “mudança grande”, nem `patch` se limita a um
arquivo pequeno. A escolha depende do efeito para outros ESPs e para os dados
persistidos. Versões `0.Y.Z` indicam desenvolvimento inicial; mesmo nessa fase,
este projeto reserva **major** para incompatibilidade intencional. Se uma mudança
se encaixa em mais de uma categoria, use a mais alta e explique no release como
atualizar os dispositivos envolvidos. Mudanças só na documentação não alteram
o firmware e, por padrão, não geram uma nova versão OTA.

O estado de referência é `versoes/estado_versoes.json`. Ele guarda a última
etiqueta preparada, a versão de cada ESP e uma impressão digital de suas fontes.
**Não edite versões manualmente** nos arquivos gerados ou no binário. O
preparador considera código compartilhado nos alvos afetados; HTML, CSS,
JavaScript, geradores das páginas e logo entram na comparação dos satélites.
Arquivos `.pio` e caches não contam como mudança de fonte.

## Preparar o conjunto no computador

Faça esta etapa na raiz do repositório, com Python e PlatformIO disponíveis.
Ela prepara arquivos **locais**; ainda não publica nada no GitHub.

1. Termine as mudanças de código e documentação. Execute os testes pertinentes
   e confira o diff. O [manual de ferramentas](ferramentas.md) lista os comandos
   de teste, compilação e geração das páginas.
2. Escolha o incremento pela tabela acima. O comando abaixo abre um menu no
   terminal: `1` para patch, `2` para minor, `3` para major e `0` para cancelar.

   ```powershell
   python ferramentas\preparar_release_ota.py
   ```

   Para automação ou terminal sem entrada interativa, informe a escolha:

   ```powershell
   python ferramentas\preparar_release_ota.py --bump minor
   ```

   Troque `minor` por `patch` ou `major` conforme o caso. `--forcar equipe-s3`
   (ou outro alvo, ou `todos`) cria nova versão mesmo sem alteração detectada;
   use apenas quando realmente precisa reconstruir e publicar outra imagem.
   `--repositorio proprietario/repositorio` muda o destino das URLs do manifesto.
3. Aguarde a conclusão. O script regenera os painéis, detecta fontes alteradas,
   incrementa as versões pertinentes, compila os alvos necessários e confere a
   versão **dentro** de cada `.bin`. Imagens anteriores só são reutilizadas quando
   correspondem à versão registrada. Ao final, imprime a etiqueta, as versões e
   os caminhos dos quatro anexos. Se nenhuma fonte mudou e a saída anterior
   está completa, ele informa isso e não aumenta a versão.
4. Confira `versoes/estado_versoes.json` e os quatro arquivos em
   `versoes/saida/`: `mestre-s3.bin`, `equipe-c3.bin`, `visitantes-c3.bin` e
   `manifesto-firmwares.json`. No JSON, cada alvo deve apontar para a **mesma
   etiqueta** e para o anexo correto, com tamanho e SHA-256. O manifesto atual
   usa `versao_formato` 2. Não misture imagens de preparações diferentes.
5. Execute `git status --short` e revise as fontes e os `painel_web.c` gerados.
   Inclua no commit o código alterado, a documentação, os painéis gerados e
   `versoes/estado_versoes.json`. `versoes/saida` e `versoes/historico` são
   artefatos locais ignorados pelo Git; não precisam entrar no commit.

Se a compilação ou a verificação da versão falhar, corrija o erro e execute o
preparador novamente. Antes de publicar, confira se o estado e os quatro anexos
formam o mesmo conjunto. `criar_manifesto_ota.py` só é necessário quando já
existem três imagens verificadas e se deseja reconstruir manualmente o JSON;
ele **não** compila nem confere a versão interna. Veja os parâmetros em
[ferramentas](ferramentas.md).

## Publicar o GitHub Release

O caminho normal usa o workflow
`.github/workflows/publicar-firmware-mestre.yml`. Ele começa quando uma tag
`vX.Y.Z` é enviada e lê as versões do **commit apontado pela tag**. Portanto:

1. Faça o commit do conjunto preparado e envie o commit ao GitHub. Confirme
   que `versoes/estado_versoes.json` e os painéis gerados estão incluídos.
2. Leia a etiqueta do estado, crie a tag **nesse commit** e envie-a. No
   PowerShell, após o commit:

   ```powershell
   $versao = (Get-Content versoes/estado_versoes.json -Raw | ConvertFrom-Json).versao_release
   $tag = "v$versao"
   git push origin HEAD
   git tag $tag
   git push origin $tag
   ```

   Se a tag já existir, investigue antes de prosseguir; não reutilize o mesmo
   nome para imagens diferentes. Um release novo precisa de uma etiqueta nova.
3. Na aba **Actions** do GitHub, acompanhe **Publicar firmwares OTA** até a
   conclusão. O fluxo testa ferramentas, regenera as páginas e rejeita fontes
   web cujo `.c` versionado esteja desatualizado. Depois recompila os três
   firmwares com as versões registradas, gera o manifesto e cria o release.
4. Na aba **Releases**, confira título, notas e os **quatro anexos**. O arquivo
   `manifesto-firmwares.json` deve indicar a tag publicada, as versões internas
   esperadas e as URLs dos três `.bin`. Edite as notas da publicação para
   descrever as mudanças e qualquer migração necessária. O script local não
   publica por si; um commit ou tag sem release concluído também não fornece
   os arquivos para OTA.

O mestre consulta `releases/latest/download/manifesto-firmwares.json`, então o
release destinado à atualização precisa ser o mais recente disponibilizado
nesse caminho. O repositório e os anexos devem estar publicamente acessíveis:
o download HTTPS atual não autentica releases privados. Se o workflow falhar,
consulte a etapa com erro em **Actions**, corrija a fonte/estado e prepare uma
nova etiqueta quando o conteúdo publicável mudar. Não instale um conjunto
parcial.

## Instalar e conferir nos ESPs

Após a [primeira gravação por cabo](#primeira-gravação-por-cabo), conecte os três
ESPs e confira no painel da Equipe se o mestre e os satélites respondem.
No terminal do mestre, abra **2 - Atualização OTA** e escolha **2 - Verificar
nova versão agora**, ou use a verificação na página **Comandos**. Consulte
versões e disponibilidade **antes** de autorizar um alvo. Pela página web, as
ações ficam disponíveis aos clientes conectados ao AP da Equipe.

Autorize mestre, Equipe, Visitantes ou **todos** no terminal ou no painel.
Em **todos**, os satélites são atualizados primeiro e o mestre por último.
Se um satélite não responder, **todos** é recusado. A autorização individual
de um satélite consulta somente seu estado pendente; portanto, a indisponibilidade
do outro satélite não a impede. Ainda é necessário que o alvo escolhido tenha
respondido à verificação, tenha atualização compatível e continue acessível
durante a transferência.
Observe porcentagem, bytes confirmados, resultado e versão que volta a
responder; uma resposta de comando apenas confirma o pedido. Aguarde também a
janela de observação da nova imagem e confira que não houve rollback. Se a
oferta expirar após 5 min sem autorização, verifique novamente. Um satélite
desligado ou sem resposta não é atualizado; restabeleça o enlace, consulte o
diagnóstico e repita a verificação antes de tentar de novo.

Em bancada, verifique também uma autorização disputando o Wi-Fi com uma
sincronização e um cancelamento durante a preparação da rede ou o download.
No primeiro caso, confira que a oferta ainda pode ser autorizada dentro do
prazo original. No segundo, confira **Cancelado** e a versão ativa do Mestre.
Após **Aplicado**, verifique o reinício e a versão informada pela nova imagem.

Se o mestre não encontrar uma atualização, confira: se o release com quatro
anexos está disponível como o mais recente; se o manifesto usa as URLs e a
etiqueta corretas; se a versão interna do alvo é realmente maior; e se Wi-Fi
externo, HTTPS e o enlace UART do satélite estão funcionando. A indicação de
erro no terminal e no painel ajuda a localizar a etapa interrompida.

Se o serial informar que o pipeline OTA dos satélites foi invalidado, uma
falha interna interrompeu a operação. Preserve os logs, reinicie o Mestre em
bancada e valide o enlace antes de tentar a transferência novamente.
