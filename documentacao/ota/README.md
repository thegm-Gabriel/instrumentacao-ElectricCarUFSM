# Atualização OTA do mestre e dos satélites

O mestre consulta periodicamente `manifesto-firmwares.json` no último GitHub
Release. O arquivo descreve separadamente os firmwares `mestre-s3`, `equipe-s3` e
`visitantes-s3`, cada um com sua própria versão, tamanho, URL e SHA-256.

O Wi-Fi permanece desligado fora das verificações e instalações. Quando há uma
atualização, o operador escolhe no terminal qual alvo autorizar. O mestre atualiza
sua própria partição por HTTPS. Para um satélite, ele baixa a imagem por HTTPS e a
retransmite pela UART em blocos confirmados individualmente.

O protocolo OTA dos satélites usa sessão, sequência, deslocamento e CRC-32. Blocos
sem confirmação são repetidos sem regravação duplicada. O satélite grava somente a
partição inativa, compara o SHA-256 completo, seleciona a nova imagem e reinicia. A
imagem nova só é confirmada depois que Wi-Fi, UART e servidor web inicializam; se
isso falhar, o bootloader retorna à versão anterior.

Durante a transferência, o terminal e a SSD1306 mostram o mesmo estado mantido
pelo serviço OTA: alvo, versão, bytes confirmados, porcentagem, taxa em MB/s e tempo
restante. A última faixa da OLED é usada como barra de progresso.

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
6 - Cancelar atualizações pendentes
7 - Alterar intervalo das verificações
```

O mestre só oferece OTA ao satélite que esteja respondendo, anuncie a capacidade
OTA e tenha uma versão menor que a descrita no manifesto.

## Preparar um release

Execute o preparador sem informar versões:

```powershell
python ferramentas\preparar_release_ota.py
```

O script calcula uma impressão digital das fontes de cada projeto e da pasta
`firmware/compartilhado`. Ele aumenta o número de correção somente dos firmwares
alterados, compila de forma incremental e confere a versão gravada dentro de cada
imagem. Uma mudança em código compartilhado versiona os três firmwares.

Os arquivos atuais ficam em `versoes/saida`:

```text
mestre-s3.bin
equipe-s3.bin
visitantes-s3.bin
manifesto-firmwares.json
```

Antes de substituir essa saída, o script move o conjunto anterior para
`versoes/historico/vVERSAO`. O controle das versões e impressões digitais fica em
`versoes/estado_versoes.json`. Esse arquivo deve ser mantido no repositório.

Para aumentar uma versão mesmo sem mudança detectada:

```powershell
python ferramentas\preparar_release_ota.py --forcar equipe-s3
python ferramentas\preparar_release_ota.py --forcar todos
```

Crie no GitHub o release com a etiqueta informada pelo script e anexe os quatro
arquivos de `versoes/saida`. O repositório precisa ser público enquanto o firmware
não implementar autenticação para downloads privados.

O fluxo `.github/workflows/publicar-firmware-mestre.yml` também foi adaptado aos
três alvos. Depois de executar o preparador e versionar
`versoes/estado_versoes.json`, envie uma tag igual à etiqueta mostrada pelo script;
o GitHub Actions recompila cada imagem com sua versão registrada e publica os
quatro arquivos no mesmo release.
