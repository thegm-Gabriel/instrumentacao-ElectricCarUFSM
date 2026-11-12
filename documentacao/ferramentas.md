# Ferramentas de desenvolvimento e publicação

[Índice da documentação](README.md) · [Versionamento e OTA](ota.md)

Execute os comandos abaixo **na raiz do repositório**. Os scripts de operação
usam Python, os testes dos painéis usam Node.js e as compilações usam
PlatformIO com ESP-IDF. Use `python ... --help` nos scripts
que aceitam argumentos para confirmar a sintaxe disponível nesta revisão.
Nenhuma ferramenta deste guia grava firmware em uma placa sem o comando de
upload explícito.

## Qual ferramenta usar

| Ferramenta | Quando usar | Entrada | Saída |
|---|---|---|---|
| `firmware/equipe-s3/web/gerar_painel.py` | Após editar a página técnica. | `index.html`, `style.css`, `layout.css`, `app.js` e o logo compartilhado. | `firmware/equipe-s3/src/servicos/painel_web.c`. |
| `firmware/visitantes-s3/web/gerar_painel.py` | Após editar a página pública. | `index.html`, `style.css`, `app.js` e o logo compartilhado. | `firmware/visitantes-s3/src/servicos/painel_web.c`. |
| `ferramentas/visualizar_painel_visitantes.py` | Ver a página pública no PC ou celular antes de gravar o ESP. | As mesmas fontes da página e telemetria simulada local. | Servidor HTTP local na porta 8765. |
| `ferramentas/visualizar_painel_equipe.py` | Experimentar o painel técnico, os comandos e os estados de saúde no PC ou celular. | Fontes da Equipe e APIs simuladas em memória. | Servidor HTTP local na porta 8766. |
| `ferramentas/gerar_segredos_ota.py` | Criar a configuração local inicial do Wi-Fi usado pelo mestre. | Variáveis `OTA_WIFI_SSID` e `OTA_WIFI_SENHA`. | `firmware/mestre-s3/include/nucleo/segredos_ota.h`. |
| `ferramentas/gerar_qr_visitantes.py` | Quando SSID, senha ou IP público dos Visitantes mudar. | Macros de `firmware/visitantes-s3/include/nucleo/configuracao.h`. | Dois PNGs e dois SVGs em `artefatos/qr_visitantes`. |
| `ferramentas/preparar_release_ota.py` | Preparar um conjunto OTA versionado. | Fontes, estado anterior, escolha de incremento e imagens anteriores quando reutilizadas. | Três `.bin`, manifesto, histórico e `estado_versoes.json`. |
| `ferramentas/criar_manifesto_ota.py` | Gerar manualmente o manifesto de três imagens já compiladas. | Caminho e versão de cada `.bin`, etiqueta e repositório. | JSON com URL, SHA-256 e tamanho por alvo. |

Os scripts de operação ficam em `ferramentas/`, cada gerador de painel junto
das fontes `web/` correspondentes, e os testes de regressão em
`ferramentas/testes/`. Nenhum script de prévia grava configurações ou firmware
nos ESPs.

## Gerar os painéis incorporados

```powershell
python firmware\equipe-s3\web\gerar_painel.py
python firmware\visitantes-s3\web\gerar_painel.py
```

As páginas são servidas como uma única resposta HTML com CSS, JavaScript e o
símbolo do projeto incorporados; não há solicitação externa para esses arquivos.
**Edite as fontes em `web/`, não o `.c` gerado.** Confira o diff dos dois lados
e inclua ambos no commit. Se o símbolo mudar, substitua
`firmware/compartilhado/web/logo_carro_eletrico.png` e execute os dois
geradores. `node --check firmware/equipe-s3/web/app.js` e o comando equivalente
de Visitantes verificam sintaxe de JavaScript; ainda é preciso testar a página
em tela estreita, resposta ausente e dados reais/simulados.

### Prévia local da página de visitantes

```powershell
python ferramentas\visualizar_painel_visitantes.py
```

Abra `http://127.0.0.1:8765/` no navegador. O servidor monta o mesmo HTML
incorporado pelo gerador e responde à API de telemetria com trajetos simulados
variados, identificados como demonstração na página. A prévia da Equipe usa a
mesma fonte local; ambas alternam cruzeiro suave e frenagens de intensidades
diferentes. Ele lê as fontes a cada abertura:
após editar HTML, CSS ou JavaScript, atualize o navegador. Use `Ctrl+C` para
encerrar. Se a porta estiver ocupada, passe `--porta 8766` e abra o endereço
correspondente. A prévia permite avaliar layout e interações; dados reais e
comportamento de rede ainda dependem do ESP.

Para abrir também no celular, conecte-o à mesma rede Wi-Fi do PC e execute:

```powershell
python ferramentas\visualizar_painel_visitantes.py --rede
```

No PC, consulte o endereço IPv4 da conexão Wi-Fi com `ipconfig`. No celular,
abra `http://IP_DO_PC:8765/`, substituindo `IP_DO_PC` por esse endereço. A
opção `--rede` faz o servidor aceitar conexões da rede local; sem ela, aceita
somente conexões do próprio PC. Se o celular não conseguir abrir, confira se a
rede permite comunicação entre dispositivos e se o firewall do Windows
permite o Python na rede privada.

### Prévia local da página da equipe

```powershell
python ferramentas\visualizar_painel_equipe.py --rede
```

No PC, abra `http://127.0.0.1:8766/`. No celular conectado à mesma rede,
abra `http://IP_DO_PC:8766/`; obtenha o IPv4 do PC com `ipconfig`. Sem `--rede`,
o acesso fica restrito ao próprio computador. `--porta 8770`, por exemplo,
permite escolher outra porta. As prévias da Equipe e de Visitantes podem rodar
ao mesmo tempo em terminais separados, usando suas portas padrão.

A ferramenta usa **as mesmas fontes HTML, CSS e JavaScript do firmware da
Equipe**. Atualize o navegador depois de editar a interface; reinicie o script
se alterar o simulador Python. Encerre com `Ctrl+C`. Não é necessário gerar o
arquivo C nem compilar para usar a prévia.

Uma faixa exclusiva da prévia identifica que **todos os dados e comandos são
simulados**, inclusive se você selecionar “Sensores reais” no painel. Essa
faixa permite alternar entre funcionamento normal, operação degradada e mestre
sem resposta. Também é possível iniciar em um cenário específico:

```powershell
python ferramentas\visualizar_painel_equipe.py --rede --cenario degradado
python ferramentas\visualizar_painel_equipe.py --cenario sem-mestre
```

O ambiente fornece telemetria a 4 Hz, diagnósticos dos três ESPs, clientes Wi-Fi,
redes salvas e configurações de exemplo. Os comandos modificam apenas o estado
em memória: a sincronização simula três arquivos em aproximadamente 12 s; a
consulta OTA oferece uma versão após 2 s e a transferência simulada leva 10 s.
Sem autorização, a oferta expira em aproximadamente 60 s. Reiniciar o script
restaura os valores iniciais; todos os navegadores conectados à mesma instância
compartilham esse estado.

Use nomes e senhas fictícios nos formulários. A ferramenta não acessa ESPs,
Google ou redes Wi-Fi, não atualiza firmware e descarta as senhas recebidas.
A permissão administrativa do **mestre** na prévia aceita um texto fictício não
vazio. Os demais comandos da Equipe não exigem chave adicional. A prévia aceita
senha vazia para simular uma rede aberta.
O simulador serve para avaliar a interface e seus estados; UART, SD, tempos
reais de transmissão e reconexões físicas ainda precisam de teste em bancada.

Para conferir o simulador após uma alteração:

```powershell
python -m unittest discover -s ferramentas/testes -p test_visualizar_painel_equipe.py -v
```

Os testes cobrem progresso e conclusão de Google/OTA, expiração de autorização,
configuração Wi-Fi, clientes, cenários sem mestre e respostas HTTP. Usam uma
porta local temporária e a liberam ao terminar.

### Testes rápidos das interfaces

```powershell
node --check firmware\equipe-s3\web\app.js
node --check firmware\visitantes-s3\web\app.js
node ferramentas\testes\test_marcador_velocidade.js
node ferramentas\testes\test_animacao_visitantes.js
node ferramentas\testes\test_pico_visitantes.js
node ferramentas\testes\test_pico_potencia_visitantes.js
```

Esses testes cobrem o marcador do velocímetro técnico, a transição numérica
do painel público, seu pico de velocidade e os picos de consumo/regeneração.
Rodam no Node.js sem ESP. Não substituem conferir tamanhos de tela, dados reais,
perda de enlace e comportamento ao toque com as prévias ou em bancada.

## Criar segredos locais do mestre

```powershell
$env:OTA_WIFI_SSID = 'nome-da-rede'
$env:OTA_WIFI_SENHA = 'senha-da-rede'
python ferramentas\gerar_segredos_ota.py
```

O script valida limites do SSID e da senha e **sobrescreve**
`segredos_ota.h` com apenas essas duas macros. Use-o na configuração inicial.
Se o arquivo já contiver URL/token Google, redes adicionais ou senha de
permissão, faça a edição a partir de
`firmware/mestre-s3/include/nucleo/segredos_ota.exemplo.h` e preserve esses
valores localmente. O cabeçalho real é ignorado pelo Git e não deve ser
publicado. A integração Google é explicada em [google.md](google.md).

## Gerar QR codes de Visitantes

```powershell
python -m pip install "qrcode[pil]"
python ferramentas\gerar_qr_visitantes.py
```

O gerador lê `WIFI_SSID`, `WIFI_SENHA` e `IP_PAINEL` do cabeçalho de
Visitantes. Cria `wifi_visitantes.png/.svg`, para conectar à rede, e
`painel_visitantes.png/.svg`, para abrir o endereço HTTP **após** conectar.
Confira o conteúdo por leitura em um celular antes de imprimir. O QR de Wi-Fi
contém a senha configurada; trate o arquivo como material de acesso público
somente se essa rede foi concebida para visitantes. Alterar o QR não muda o
firmware: edite o cabeçalho, regenere os códigos e recompile o satélite.

## Compilar, testar e gravar

```powershell
pio run -d firmware\mestre-s3 -e mestre-s3-n16r8
pio run -d firmware\equipe-s3 -e esp32-s3-devkitm-1
pio run -d firmware\visitantes-s3 -e esp32-s3-devkitm-1
python -m unittest discover -s ferramentas/testes -p test_preparar_release_ota.py -v
```

Se `pio` não estiver no PATH no Windows, use o executável da instalação local:

```powershell
$pioLocal = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
& $pioLocal run -d firmware\visitantes-s3 -e esp32-s3-devkitm-1
```

O preparador de release procura esse caminho automaticamente.

Acrescente `-t upload` ao comando `pio run` do alvo desejado para gravar **por
cabo**. Escolha a porta serial conforme sua máquina. A primeira instalação
precisa das tabelas de partição OTA; veja [OTA](ota.md). `pio run` simples
compila para validação e não cria automaticamente um conjunto publicável com
manifesto. O teste Python verifica a regra de incremento; testes de protocolo
e validação em bancada têm escopos próprios em [protocolos](protocolos.md).

## Preparar um release OTA

Para escolher o incremento em um menu no terminal:

```powershell
python ferramentas\preparar_release_ota.py
```

Digite `1` para **patch**, `2` para **minor**, `3` para **major** ou `0` para
cancelar antes de compilar e modificar arquivos. Uma opção inválida pede nova
escolha. O menu só aparece em um terminal interativo; para scripts e CI,
informe `--bump patch`, `--bump minor` ou `--bump major` explicitamente. Esse
argumento dispensa o menu. `--forcar` aceita
`mestre-s3`, `equipe-s3`, `visitantes-s3` ou `todos`; `--repositorio` troca o
destino GitHub no manifesto. Exemplos:

```powershell
python ferramentas\preparar_release_ota.py --bump minor
python ferramentas\preparar_release_ota.py --bump patch --forcar equipe-s3
python ferramentas\preparar_release_ota.py --bump patch --repositorio usuario/repositorio
```

O script compara impressões digitais, incrementa a etiqueta e apenas os
firmwares alterados, compila incrementalmente, confere a versão dentro de cada
imagem, gera o manifesto e arquiva a saída anterior em `versoes/historico`.
Antes da comparação, ele regenera os dois painéis web; HTML, CSS, JavaScript,
geradores Python e o logo compartilhado entram na impressão digital dos
respectivos satélites. Uma mudança visual só incrementa o firmware afetado.
Revise e inclua no commit o `painel_web.c` regenerado: o fluxo de publicação
executa os geradores e recusa a tag se o resultado divergir do arquivo no Git.
O fluxo também executa os testes Python e JavaScript disponíveis antes de
compilar os três ESPs.
O conjunto novo fica em `versoes/saida`. Não apague `versoes/estado_versoes.json`:
ele informa o último conjunto preparado e pertence ao Git. Para escolher o
incremento, criar a tag, acompanhar o workflow e instalar os três alvos,
siga o [procedimento de release e OTA](ota.md#escolher-a-versão).

## Gerar um manifesto manualmente

Use somente quando já possui três imagens com versões internas verificadas e
precisa reconstruir o JSON sem executar o preparador:

```powershell
python ferramentas\criar_manifesto_ota.py `
  --firmware-mestre versoes\saida\mestre-s3.bin `
  --firmware-equipe versoes\saida\equipe-s3.bin `
  --firmware-visitantes versoes\saida\visitantes-s3.bin `
  --versao-mestre 2.3.0 --versao-equipe 1.7.2 --versao-visitantes 1.7.2 `
  --etiqueta v2.4.6 --repositorio usuario/repositorio `
  --saida versoes\saida\manifesto-firmwares.json
```

Os números são **exemplo**, não valores a copiar cegamente. O script verifica
que cada arquivo se parece com uma imagem ESP32 e valida o hash anexado quando
presente. Ele escreve URL, tamanho e SHA-256, mas **não** compila, não compara a
versão interna do binário com `--versao-*` e não publica no GitHub. Uma etiqueta
errada ou anexos diferentes dos usados no cálculo inviabilizam OTA; prefira o
preparador normal.
