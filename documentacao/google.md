# Histórico de telemetria no Google

[Índice da documentação](README.md) · [Pinagem do microSD](pinagem.md#cartão-microsd-do-mestre) · [Firmware mestre](mestre.md#armazenamento-e-envio-ao-google)

O mestre publica telemetria ao vivo aos satélites a **4 Hz** e guarda uma
amostra por segundo em arquivos CSV no microSD. O envio ao Google é uma
operação **manual**, iniciada no terminal do mestre ou na página **Comandos**
do ESP da Equipe. O painel de Visitantes não controla essa operação.

O destino consiste em uma pasta no Drive, que recebe os CSVs originais, e uma
planilha dentro dessa pasta, com uma aba por sessão do mestre. O arquivo local
só deixa a lista de pendentes depois que o Web App confirma **aquele nome de
arquivo**. A ligação elétrica do cartão, os GPIOs e os níveis de tensão estão
apenas no [guia de pinagem](pinagem.md#cartão-microsd-do-mestre).

## Preparar a conta e o Apps Script

Esta integração usa um [Web App do Google Apps Script](https://developers.google.com/apps-script/guides/web),
sem configurar manualmente um projeto Google Cloud ou um cliente OAuth. O
script executa com as permissões da conta que o implantou; o mestre usa um
token compartilhado para autorizar cada envio.

1. Entre na conta Google que guardará o histórico. Crie uma pasta no Drive,
   por exemplo `Telemetria Carro`. Abra a pasta e copie da URL a parte que vem
   depois de `/folders/`: esse é o **ID da pasta**, não seu nome.
2. Em [script.google.com](https://script.google.com/), crie um projeto e
   substitua o conteúdo de `Code.gs` pelo arquivo
   [`telemetria-google/Code.gs`](telemetria-google/Code.gs) deste repositório.
3. Em **Configurações do projeto → Propriedades do script**, salve as duas
   propriedades abaixo. Os nomes devem ser escritos exatamente assim:

   | Propriedade | Valor |
   |---|---|
   | `ID_PASTA_TELEMETRIA` | ID da pasta copiado no passo 1. |
   | `TOKEN_TELEMETRIA` | Segredo aleatório longo escolhido pela equipe; use pelo menos 32 caracteres. |

   `ID_PLANILHA_TELEMETRIA` é salva pelo próprio script quando ele abre ou cria
   a planilha pela primeira vez; não é preciso preenchê-la.
4. Em **Implantar → Nova implantação → Aplicativo da Web**, escolha **Executar
   como: Eu** e **Quem pode acessar: Qualquer pessoa**. Essa opção permite ao
   ESP enviar sem uma sessão Google no navegador; **Qualquer pessoa com uma
   Conta do Google** não atende ao firmware. Autorize o acesso solicitado a
   Drive e Sheets e copie a URL publicada terminada em `/exec`.
5. No mestre, copie
   [`segredos_ota.exemplo.h`](../firmware/mestre-s3/include/nucleo/segredos_ota.exemplo.h)
   para `firmware/mestre-s3/include/nucleo/segredos_ota.h`. Configure uma rede
   Wi-Fi com internet e adicione a URL `/exec` e o mesmo token das propriedades:

   ```c
   #define TELEMETRIA_GOOGLE_URL "https://script.google.com/macros/s/SEU_ID/exec"
   #define TELEMETRIA_GOOGLE_TOKEN "o-mesmo-token-das-propriedades-do-script"
   ```

   O cabeçalho real é local e ignorado pelo Git. O script
   `ferramentas/gerar_segredos_ota.py` cria **somente** as macros iniciais de
   Wi-Fi e sobrescreve esse arquivo; se ele já contém a URL e o token, edite-o
   diretamente para não perder a configuração Google. Depois, compile e grave
   o mestre conforme o [guia de ferramentas](ferramentas.md).

**Ao alterar `Code.gs`:** salvar o código não atualiza automaticamente a
implantação `/exec`. Abra **Implantar → Gerenciar implantações**, escolha a
implantação existente, clique **Editar**, selecione **Nova versão** no campo
de versão e confirme **Implantar**. Assim a URL usada pelo mestre é preservada.
Uma URL `/dev` pertence ao teste do editor e não é a
implantação a configurar no firmware. As [orientações do Google para versões e
implantações](https://developers.google.com/apps-script/concepts/deployments)
descrevem essa distinção.

## O que fica no cartão

O serviço de registro é separado da transmissão ao vivo. Uma fila de até 24
amostras evita que uma escrita lenta pare a aquisição; se ela encher, a perda
é contabilizada no diagnóstico. O CSV ativo é sincronizado com o cartão a
cada 10 s e fechado após **1 h ou 8 MiB**, o que ocorrer primeiro. Esses
valores estão em
[`configuracao_registro_telemetria.h`](../firmware/mestre-s3/include/nucleo/configuracao_registro_telemetria.h).

| Local no microSD | Significado |
|---|---|
| `/sdcard/telemetria/telemetria_XXXXXXXX_NNNN.tmp` | Arquivo que o mestre ainda está escrevendo; não entra no envio. `XXXXXXXX` identifica a sessão do boot e `NNNN` a sequência do arquivo, não uma data. |
| `/sdcard/telemetria/telemetria_XXXXXXXX_NNNN.csv` | Arquivo fechado e pendente de confirmação do Google. |
| `/sdcard/telemetria/enviados/` | CSVs cujo nome foi confirmado pelo Web App e que puderam ser movidos no cartão. São preservados localmente. |
| `/sdcard/telemetria/*.incompleto` | Arquivo interrompido que não passou na verificação de recuperação; é preservado para análise e não é enviado automaticamente. |

Na partida, o mestre examina arquivos `.tmp`: quando o cabeçalho esperado está
presente e o arquivo termina em uma linha completa, ele o transforma em `.csv`
pendente. Caso contrário, tenta preservá-lo como `.incompleto`. Um arquivo em
uso também é fechado antes de começar uma sincronização solicitada, para que
as amostras já registradas possam entrar no lote.

O CSV traz `tempo_sessao_ms` (tempo do mestre em milissegundos desde o boot),
leituras disponíveis, campos ainda sem sensor físico e a coluna `simulado`.
**Zero não comprova medição real** para um campo ainda não instrumentado. Os
gráficos do navegador guardam apenas as amostras recebidas naquela página;
não substituem o CSV do cartão. Logs e cópias de configuração do SD **não**
são enviados por esta integração.

Sem cartão montado, o histórico CSV e o envio ficam indisponíveis, mas a
telemetria ao vivo continua. O mestre tenta recuperar o registro quando o
cartão volta a estar disponível; consulte [diagnóstico e recuperação](mestre.md#armazenamento-e-envio-ao-google).

## Solicitar e acompanhar a sincronização

1. Confira que o microSD está disponível e que o mestre possui uma rede Wi-Fi
   externa com acesso à internet. A lista de pendentes e o estado do registro
   aparecem no diagnóstico da Equipe.
2. No terminal serial do mestre, a 115200 baud, escolha a operação de enviar
   CSVs pendentes ao Google. Como alternativa, conecte-se à rede da Equipe,
   abra `http://192.168.4.1/`, prepare o acesso com a chave administrativa e,
   em **Comandos → Sincronização com Google**, clique **Sincronizar arquivos
   pendentes**. O botão **Atualizar estado** apenas consulta; não inicia envio.
3. A resposta inicial confirma que o pedido foi aceito ou recusado; **não**
   confirma que os CSVs chegaram ao Google. Aguarde o estado final. O painel
   atualiza automaticamente o andamento e mostra arquivo atual, bytes,
   porcentagem, velocidade estimada, tempo restante, número de arquivos
   concluídos e resultado. Se não houver `.csv` pendente, a operação termina
   com sucesso sem transmitir arquivo.
4. Confira que os arquivos desapareceram da lista de pendentes, apareceram
   em `telemetria/enviados` e estão na pasta e na planilha Google. Uma resposta
   final de erro exige verificação dos arquivos que **ainda** estão pendentes.

O mestre não inicia duas sincronizações simultâneas nem toma o Wi-Fi enquanto
outra sessão de rede o ocupa. Durante uma execução, ele fecha o CSV ativo, abre
uma sessão Wi-Fi temporária e percorre os `.csv` pendentes. Para cada arquivo,
envia um `POST` HTTPS com o conteúdo CSV e os parâmetros `arquivo` e `token`.
O Google pode redirecionar a leitura da resposta para
`script.googleusercontent.com`; o firmware segue esse retorno e só aceita
**HTTP 200 com o texto exato `OK:<nome-do-arquivo>`**. Em caso de falha, tenta
o mesmo arquivo até quatro vezes, com esperas crescentes. Se ainda falhar,
encerra o lote e deixa os não confirmados no cartão. Depois da confirmação,
move o CSV para `enviados` e continua com o próximo. A sessão Wi-Fi é
encerrada ao final.

## O que o Google cria e como evita duplicatas

O Apps Script grava cada CSV original na pasta escolhida e mantém **uma
planilha** chamada `telemetria_carro` nessa mesma pasta. Cada identificador de
sessão recebe uma aba `sessao_XXXXXXXX`. A primeira coluna da aba,
`arquivo_origem`, guarda o nome do CSV que originou cada linha; as outras
colunas seguem o cabeçalho do arquivo.

Antes de inserir linhas, o script procura o nome do arquivo na aba daquela
sessão. Antes de criar o CSV no Drive, procura um arquivo com o mesmo nome na
pasta. Isso permite repetir um envio após perda de resposta sem inserir as
mesmas linhas ou criar outro CSV. Se o Google confirmar o arquivo mas o mestre
falhar ao movê-lo para `enviados`, ele continua pendente localmente; uma nova
tentativa deve receber a mesma confirmação sem duplicar os dados remotos.
Planilhas criadas por versões antigas do script não são apagadas nem migradas
automaticamente.

## Investigar falhas

| Sinal observado | O que conferir |
|---|---|
| Botão indisponível ou pedido recusado | Estado do mestre, cartão e serviço de registro; sincronização ou outra sessão Wi-Fi já em andamento; chave administrativa da Equipe. |
| Nenhum progresso e zero pendentes | A execução não encontrou CSVs com amostras para enviar. Consulte o resultado final e o contador de registro antes de tentar novamente. |
| Falha de rede, DNS ou TLS | Credenciais da rede externa, acesso à internet, sinal Wi-Fi e estado da sessão no diagnóstico/serial do mestre. |
| `ERRO:configuracao` | Falta `ID_PASTA_TELEMETRIA` ou `TOKEN_TELEMETRIA` nas propriedades do script. |
| `ERRO:autorizacao` | O token do firmware difere de `TOKEN_TELEMETRIA`. |
| `ERRO:nome`, `ERRO:conteudo` ou `ERRO:csv` | Nome fora do padrão esperado, corpo vazio ou CSV sem ao menos cabeçalho e uma linha; examine o arquivo preservado no SD. |
| `ERRO:interno`, HTTP inesperado ou redirecionamento ausente | Veja **Execuções** no Apps Script, permissões de Drive/Sheets, pasta, implantação `/exec` e o código HTTP no serial. |
| Google confirmou, mas arquivo continua pendente | Verifique o erro de movimentação no SD. Repetir o envio é seguro para o mesmo nome, devido à conferência de duplicatas do script. |

As respostas `ERRO:*` são texto do Web App; uma resposta HTTP por si só não é
confirmação de recebimento. O mestre registra no serial o erro de transporte,
o código HTTP e a resposta recebida. O painel mostra o estado agregado e o
último resultado; o log estruturado do SD contém o contexto da falha quando
disponível. Não publique a URL completa de envio com `token` em relatórios ou
capturas de tela. O acesso ao Web App é aberto para permitir o ESP sem login;
**o token é a autorização efetiva** e deve permanecer apenas nas propriedades
do script e no cabeçalho local do mestre.

## Conferência inicial em bancada

1. Com o microSD e a rede configurados, deixe o mestre registrar algumas
   amostras. Confira no diagnóstico que o cartão está disponível e que o
   contador de amostras gravadas aumentou.
2. Inicie uma sincronização e espere o resultado final, não apenas a resposta
   ao comando. Confirme o arquivo no Drive, as linhas na aba da sessão e a
   cópia em `telemetria/enviados`.
3. Se houver falha, preserve o CSV pendente e compare painel, serial do mestre
   e **Execuções** do Apps Script antes de repetir. Não remova arquivos do
   cartão para “destravar” o envio sem entender a causa.
