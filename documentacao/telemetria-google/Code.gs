// Recebe CSVs do mestre, arquiva o original no Drive e consolida cada sessão no Sheets.
function doPost(evento) {
  const parametros = evento && evento.parameter ? evento.parameter : {};
  const propriedades = PropertiesService.getScriptProperties();
  const tokenEsperado = propriedades.getProperty('TOKEN_TELEMETRIA');
  const idPasta = propriedades.getProperty('ID_PASTA_TELEMETRIA');
  const nomeArquivo = String(parametros.arquivo || '');
  const tokenRecebido = String(parametros.token || '');

  if (!tokenEsperado || !idPasta) {
    return resposta_('ERRO:configuracao');
  }
  if (tokenRecebido !== tokenEsperado) {
    return resposta_('ERRO:autorizacao');
  }
  if (!/^telemetria_[0-9a-f]{8}_[0-9]{4}\.csv$/i.test(nomeArquivo)) {
    return resposta_('ERRO:nome');
  }

  const csv = evento.postData && evento.postData.contents
    ? evento.postData.contents
    : '';
  if (!csv) {
    return resposta_('ERRO:conteudo');
  }

  const trava = LockService.getScriptLock();
  trava.waitLock(30000);
  try {
    const pasta = DriveApp.getFolderById(idPasta);
    const linhas = Utilities.parseCsv(csv);
    if (linhas.length < 2) {
      return resposta_('ERRO:csv');
    }

    const sessao = nomeArquivo.match(/^telemetria_([0-9a-f]{8})_/i)[1];
    const planilha = obterPlanilhaDaSessao_(pasta, sessao);
    const aba = planilha.getSheets()[0];
    const jaInserido = aba.getLastRow() > 1 &&
      aba.getRange(2, 1, aba.getLastRow() - 1, 1)
        .createTextFinder(nomeArquivo)
        .matchEntireCell(true)
        .findNext() !== null;

    if (!jaInserido) {
      if (aba.getLastRow() === 0) {
        aba.getRange(1, 1, 1, linhas[0].length + 1)
          .setValues([['arquivo_origem'].concat(linhas[0])]);
      }
      const dados = linhas.slice(1).map(linha => [nomeArquivo].concat(linha));
      aba.getRange(aba.getLastRow() + 1, 1, dados.length, dados[0].length)
        .setValues(dados);
      SpreadsheetApp.flush();
    }

    if (!pasta.getFilesByName(nomeArquivo).hasNext()) {
      pasta.createFile(nomeArquivo, csv, MimeType.CSV);
    }
    return resposta_('OK:' + nomeArquivo);
  } catch (erro) {
    console.error(erro);
    return resposta_('ERRO:interno');
  } finally {
    trava.releaseLock();
  }
}

function obterPlanilhaDaSessao_(pasta, sessao) {
  const nome = 'planilha_telemetria_' + sessao;
  const arquivos = pasta.getFilesByName(nome);
  if (arquivos.hasNext()) {
    return SpreadsheetApp.openById(arquivos.next().getId());
  }
  const planilha = SpreadsheetApp.create(nome);
  planilha.getSheets()[0].setName('dados');
  DriveApp.getFileById(planilha.getId()).moveTo(pasta);
  return planilha;
}

function resposta_(texto) {
  return ContentService.createTextOutput(texto)
    .setMimeType(ContentService.MimeType.TEXT);
}
