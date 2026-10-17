# Telemetria diária no Google

O mestre transmite telemetria ao vivo aos satélites a 4 Hz e registra uma cópia
histórica independente a 1 Hz. Os CSVs são gravados no cartão SD durante o uso e
só são enviados quando a equipe escolhe a opção **Enviar telemetria pendente ao
Google** no terminal.

## Preparação com uma conta Google pessoal

Esta integração não usa um projeto do Google Cloud nem exige a configuração
manual de OAuth. A autorização do Drive e do Sheets ocorre uma única vez no
próprio Apps Script; o ESP autentica cada envio com um token privado escolhido
pela equipe.

1. Entre apenas com a conta pessoal que receberá a telemetria. Crie no Google
   Drive uma pasta chamada `Telemetria Carro` e copie da URL o texto que aparece
   depois de `/folders/`; esse é o ID da pasta.
2. Acesse [script.google.com](https://script.google.com/), crie um projeto e
   cole nele todo o conteúdo do arquivo `Code.gs` desta pasta.
3. Abra **Configurações do projeto > Propriedades do script** e crie:

   - `ID_PASTA_TELEMETRIA`: o ID copiado no passo 1;
   - `TOKEN_TELEMETRIA`: um texto aleatório longo, com pelo menos 32 caracteres.

4. Selecione **Implantar > Nova implantação > Aplicativo da Web**. Configure:

   - **Executar como:** Eu;
   - **Quem pode acessar:** Qualquer pessoa.

   Confirme as permissões solicitadas para Drive e Sheets. A opção precisa
   permitir acesso sem login; `Qualquer pessoa com uma Conta do Google` não
   funciona para o ESP.
5. Copie a URL da implantação terminada em `/exec`. Não use a URL de teste
   terminada em `/dev`.
6. Coloque essa URL e exatamente o mesmo token em
   `firmware/mestre-s3/include/nucleo/segredos_ota.h`:

```c
#define TELEMETRIA_GOOGLE_URL "https://script.google.com/macros/s/SEU_ID/exec"
#define TELEMETRIA_GOOGLE_TOKEN "o-mesmo-token-das-propriedades-do-script"
```

O arquivo `segredos_ota.h` é local e está no `.gitignore`. Mantenha no
repositório somente `segredos_ota.exemplo.h`, sem credenciais válidas.

Ao alterar o `Code.gs`, crie uma nova versão da implantação em **Gerenciar
implantações > Editar**. Apenas salvar o código não atualiza a URL `/exec` já
publicada.

O script cria uma única planilha chamada `telemetria_carro` dentro da pasta e
organiza cada sessão do mestre em uma folha chamada `sessao_XXXXXXXX`. O ID dessa
planilha é salvo automaticamente na propriedade `ID_PLANILHA_TELEMETRIA`; não é
necessário configurá-la manualmente. Os CSVs originais continuam arquivados na
pasta.

O nome do arquivo identifica cada lote. Antes de inserir as linhas, o script
procura esse identificador apenas na folha da sessão correspondente, tornando
segura a repetição após uma queda de rede. Planilhas criadas por versões antigas
do script não são apagadas nem migradas automaticamente.

O envio não é automático durante o percurso. Ao fim da operação, use **Enviar
telemetria pendente ao Google** no terminal ou em `http://painel.mestre`. O
mestre fecha o lote atual, abre uma sessão Wi-Fi temporária e processa os arquivos
pendentes. Se a rede ou o Google falhar, o CSV permanece no cartão para uma nova
tentativa.

## Cartão SD

O leitor usa SPI2 e deve trabalhar em 3,3 V:

| Sinal | GPIO sugerido |
|---|---:|
| CLK | 38 |
| MOSI | 39 |
| MISO | 40 |
| CS | 41 |

Os GPIOs ficam em `configuracao_registro_telemetria.h` e devem ser confirmados
antes da montagem final. Arquivos `.tmp` são recuperados na próxima inicialização;
arquivos confirmados pelo Google são movidos para `telemetria/enviados` e
preservados no cartão.
