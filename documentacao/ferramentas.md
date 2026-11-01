# Ferramentas de desenvolvimento e publicação

[Índice da documentação](README.md) · [Versionamento](versionamento.md)

Execute os comandos abaixo **na raiz do repositório**. Os scripts usam Python;
as compilações usam PlatformIO com ESP-IDF. Use `python ... --help` nos scripts
que aceitam argumentos para confirmar a sintaxe disponível nesta revisão.
Nenhuma ferramenta deste guia grava firmware em uma placa sem o comando de
upload explícito.

## Qual ferramenta usar

| Ferramenta | Quando usar | Entrada | Saída |
|---|---|---|---|
| `firmware/equipe-s3/web/gerar_painel.py` | Após editar a página técnica. | `index.html`, `style.css`, `layout.css`, `app.js` e o logo compartilhado. | `firmware/equipe-s3/src/servicos/painel_web.c`. |
| `firmware/visitantes-s3/web/gerar_painel.py` | Após editar a página pública. | `index.html`, `style.css`, `app.js` e o logo compartilhado. | `firmware/visitantes-s3/src/servicos/painel_web.c`. |
| `ferramentas/gerar_segredos_ota.py` | Criar a configuração local inicial do Wi-Fi usado pelo mestre. | Variáveis `OTA_WIFI_SSID` e `OTA_WIFI_SENHA`. | `firmware/mestre-s3/include/nucleo/segredos_ota.h`. |
| `ferramentas/gerar_qr_visitantes.py` | Quando SSID, senha ou IP público dos Visitantes mudar. | Macros de `firmware/visitantes-s3/include/nucleo/configuracao.h`. | Dois PNGs e dois SVGs em `artefatos/qr_visitantes`. |
| `ferramentas/preparar_release_ota.py` | Preparar um conjunto OTA versionado. | Fontes, estado anterior, escolha de incremento e imagens anteriores quando reutilizadas. | Três `.bin`, manifesto, histórico e `estado_versoes.json`. |
| `ferramentas/criar_manifesto_ota.py` | Gerar manualmente o manifesto de três imagens já compiladas. | Caminho e versão de cada `.bin`, etiqueta e repositório. | JSON com URL, SHA-256 e tamanho por alvo. |
| `ferramentas/test_preparar_release_ota.py` | Conferir as regras de incremento SemVer. | Casos de teste locais. | Resultado `unittest`; não compila firmware. |

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
python -m unittest ferramentas.test_preparar_release_ota
```

Acrescente `-t upload` ao comando `pio run` do alvo desejado para gravar **por
cabo**. Escolha a porta serial conforme sua máquina. A primeira instalação
precisa das tabelas de partição OTA; veja [OTA](ota.md). `pio run` simples
compila para validação e não cria automaticamente um conjunto publicável com
manifesto. O teste Python verifica a regra de incremento; testes de protocolo
e validação em bancada têm escopos próprios em [protocolos](protocolos.md).

## Preparar um release OTA

```powershell
python ferramentas\preparar_release_ota.py --bump patch
```

`--bump` aceita `major`, `minor` ou `patch` (padrão). `--forcar` aceita
`mestre-s3`, `equipe-s3`, `visitantes-s3` ou `todos`; `--repositorio` troca o
destino GitHub no manifesto. Exemplos:

```powershell
python ferramentas\preparar_release_ota.py --bump minor
python ferramentas\preparar_release_ota.py --bump patch --forcar equipe-s3
python ferramentas\preparar_release_ota.py --repositorio usuario/repositorio
```

O script compara impressões digitais, incrementa a etiqueta e apenas os
firmwares alterados, compila incrementalmente, confere a versão dentro de cada
imagem, gera o manifesto e arquiva a saída anterior em `versoes/historico`.
O conjunto novo fica em `versoes/saida`. Não apague `versoes/estado_versoes.json`:
ele informa o último conjunto preparado e pertence ao Git. Para escolher o
incremento e publicar os quatro anexos, siga [versionamento](versionamento.md).

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
