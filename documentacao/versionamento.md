# Versionamento dos firmwares e do release OTA

[Índice da documentação](README.md) · [Procedimento OTA](ota.md) · [Ferramentas](ferramentas.md)

O projeto usa **SemVer**, escrito `MAJOR.MINOR.PATCH`, para a etiqueta do
release e para cada um dos três firmwares. São quatro números independentes:
uma versão do conjunto publicado e uma versão interna por dispositivo. Não
edite esses números manualmente nos arquivos gerados; use
`ferramentas/preparar_release_ota.py`.

## O significado de cada parte

| Parte | O que significa | Quando usar neste projeto | Exemplo a partir de `2.4.6` |
|---|---|---|---|
| **Major** (`X`) | Mudança incompatível que exige migração ou atualização coordenada. Zera minor e patch. | Quebra deliberada de protocolo UART/API ou de formato persistido sem compatibilidade, mudança que impede firmwares antigos de trabalhar com novos, ou revisão de hardware que exige nova instalação. | `3.0.0` |
| **Minor** (`Y`) | Nova capacidade compatível com o que já funciona. Zera patch. | Novo sensor, operação, diagnóstico, fluxo de interface ou campo versionado que preserva os consumidores anteriores. | `2.5.0` |
| **Patch** (`Z`) | Correção ou refinamento compatível. | Correção de erro, melhoria visual, texto, desempenho ou robustez sem mudar o contrato esperado. | `2.4.7` |

`major` não quer dizer “release grande” nem `patch` quer dizer “alterar um
arquivo pequeno”. A escolha depende do efeito sobre quem usa o firmware e sobre
outros dispositivos. Em SemVer, versões `0.Y.Z` indicam desenvolvimento inicial
e não prometem API estável. A convenção deste projeto, mesmo nessa fase, é
reservar **major** para uma incompatibilidade deliberada que exija coordenação.
O preparador aplica exatamente a regra de incremento escolhida.
Registre no release note qualquer migração necessária, especialmente se usar
`major`.

## Release conjunto e versões por ESP

`versoes/estado_versoes.json` registra o **último conjunto preparado**: a
etiqueta `versao_release` e, para mestre, Equipe e Visitantes, versão interna e
impressão digital das fontes. A etiqueta identifica a publicação no GitHub; o
firmware informa sua própria versão no diagnóstico e no manifesto OTA. Portanto
um release pode, corretamente, conter mestre `2.3.0` e satélites `1.7.2` se só
os satélites mudaram. O mestre não recebe uma versão nova só para igualar os
números.

O preparador regenera os painéis da Equipe e de Visitantes antes de calcular
SHA-256 das fontes. HTML, CSS, JavaScript, o gerador Python, o logo e o C
incorporado entram na impressão digital do respectivo satélite. Se apenas
Equipe mudou, incrementa Equipe e a etiqueta do release; copia as imagens
anteriores dos demais alvos quando continuam válidas. Uma mudança em código
compartilhado usado pelos três projetos é detectada nos três; o logo afeta
somente os satélites. Arquivos de build `.pio`, saídas e caches não entram no
cálculo. Revise e inclua no commit o `.c` regenerado: a publicação automática
recusa uma tag quando ele diverge das fontes web.

O preparador passa `VERSAO_FIRMWARE` ao CMake/PlatformIO, força a reconfiguração
necessária e lê a versão interna do `.bin` antes de aceitá-lo. Os binários e o
manifesto são montados temporariamente; só após a validação o conjunto anterior
é arquivado e o novo passa a `versoes/saida`. O estado versionado é atualizado
ao final. Se uma preparação falhar, corrija a causa e confira o estado e a
saída antes de repetir; não publique um manifesto misturado com binários de
outras versões.

## Preparar e publicar

1. Conclua código e documentação. O preparador gera as duas páginas antes da
   comparação; confira o diff dos arquivos incorporados.
   Rode os testes pertinentes e compile os alvos afetados. Consulte
   [ferramentas](ferramentas.md) para os comandos.
2. Escolha `major`, `minor` ou `patch` pela tabela acima. Em um terminal,
   execute sem `--bump` para abrir o menu (1: patch, 2: minor, 3: major,
   0: cancelar):

   ```powershell
   python ferramentas\preparar_release_ota.py
   ```

   Para automação ou execução sem terminal interativo, informe o incremento
   com `--bump patch`, `--bump minor` ou `--bump major`; isso ignora o menu.
   Uma migração incompatível exige coordenação da instalação dos três
   dispositivos. `--forcar equipe-s3` ou `--forcar todos` incrementa mesmo sem
   mudança detectada; use somente quando a reconstrução de fato precisa de
   uma nova versão. `--repositorio proprietario/repositorio` altera o destino
   das URLs no manifesto.
3. Confira as versões e a etiqueta impressas pelo script e os quatro arquivos
   em `versoes/saida`: `mestre-s3.bin`, `equipe-s3.bin`,
   `visitantes-s3.bin` e `manifesto-firmwares.json`. O manifesto informa URL,
   tamanho e SHA-256 por alvo. Confira que cada URL aponta para **a mesma
   etiqueta** e para o nome do anexo correspondente.
4. Revise `versoes/estado_versoes.json`, registre a alteração no Git e crie uma
   tag exatamente igual à etiqueta anunciada (`vX.Y.Z`). Publique os quatro
   arquivos juntos no GitHub Release. O fluxo
   `.github/workflows/publicar-firmware-mestre.yml` também aceita essa tag,
   executa os testes, verifica a geração das páginas e recompila/publica os
   três firmwares com as versões registradas.
5. Na primeira instalação, grave por cabo as tabelas de partição OTA. Depois,
   faça o mestre consultar o manifesto, autorize o alvo e acompanhe o estado
   até a confirmação. Veja [OTA](ota.md) para timeout e rollback.

O script não publica no GitHub por si. Um commit também não é um release: a
etiqueta, os anexos e o manifesto precisam existir juntos para que o mestre
consiga baixar as imagens. As saídas locais e o histórico são artefatos de
publicação; o arquivo de estado é mantido no Git. Não copie um `.bin` gerado
por compilação comum como anexo de uma versão preparada sem verificar sua
versão interna.

## Exemplos de decisão

- Corrigir espaçamento do painel e um prazo de consulta, preservando o
  protocolo: **patch**.
- Adicionar leitura de um novo sensor e apresentá-la com campo versionado que
  os firmwares anteriores conseguem ignorar: **minor**.
- Substituir o enquadramento UART sem leitura da revisão anterior: **major**,
  com plano de atualização coordenada ou primeira gravação por cabo.

Se a alteração envolver mais de uma categoria, escolha a mais alta e explique
no release note quais dispositivos precisam ser atualizados em conjunto.
