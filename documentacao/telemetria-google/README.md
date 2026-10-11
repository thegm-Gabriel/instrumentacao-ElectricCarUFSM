# Telemetria diária no Google

O mestre transmite telemetria ao vivo aos satélites a 4 Hz e registra uma cópia
histórica independente a 1 Hz. Os CSVs são gravados no cartão SD durante o uso e
só são enviados quando a equipe escolhe a opção **Enviar telemetria pendente ao
Google** no terminal.

## Preparação do Google Apps Script

1. Crie uma pasta no Google Drive e copie o identificador presente na URL.
2. Crie um projeto do Google Apps Script e cole nele o arquivo `Code.gs` desta
   pasta.
3. Nas propriedades do script, crie `ID_PASTA_TELEMETRIA` com o identificador da
   pasta e `TOKEN_TELEMETRIA` com um token longo escolhido pela equipe.
4. Implante o projeto como aplicativo da Web, executado pela conta proprietária,
   e escolha **Qualquer pessoa** em quem pode acessar. O token compartilhado é
   quem autoriza o ESP; nenhuma conta Google precisa existir no dispositivo.
5. Copie a URL terminada em `/exec` e coloque-a, junto com o mesmo token, em
   `firmware/mestre-s3/include/nucleo/segredos_ota.h`:

```c
#define TELEMETRIA_GOOGLE_URL "https://script.google.com/macros/s/SEU_ID/exec"
#define TELEMETRIA_GOOGLE_TOKEN "seu-token"
```

O arquivo `segredos_ota.h` é local e está no `.gitignore`. Mantenha no
repositório somente `segredos_ota.exemplo.h`, sem credenciais válidas.

Cada sessão do mestre produz uma planilha própria e mantém os CSVs originais na
pasta. O nome do arquivo identifica cada lote. O script procura esse identificador
antes de inserir as linhas, tornando segura a repetição após uma queda de rede.

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
