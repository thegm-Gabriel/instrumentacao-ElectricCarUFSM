// Recebe CSVs do mestre, arquiva o original no Drive e consolida as sessões
// em folhas separadas de uma única planilha.
const NOME_PLANILHA_TELEMETRIA = 'telemetria_carro';
const PROPRIEDADE_ID_PLANILHA = 'ID_PLANILHA_TELEMETRIA';

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
    const planilha = obterPlanilhaTelemetria_(pasta, propriedades);
    const aba = obterAbaDaSessao_(planilha, sessao);
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

function obterPlanilhaTelemetria_(pasta, propriedades) {
  const idConhecido = propriedades.getProperty(PROPRIEDADE_ID_PLANILHA);
  if (idConhecido) {
    try {
      return SpreadsheetApp.openById(idConhecido);
    } catch (erro) {
      console.warn('Planilha configurada não está acessível: ' + erro);
      propriedades.deleteProperty(PROPRIEDADE_ID_PLANILHA);
    }
  }

  const arquivos = pasta.getFilesByName(NOME_PLANILHA_TELEMETRIA);
  while (arquivos.hasNext()) {
    const arquivo = arquivos.next();
    if (arquivo.getMimeType() === MimeType.GOOGLE_SHEETS) {
      propriedades.setProperty(PROPRIEDADE_ID_PLANILHA, arquivo.getId());
      return SpreadsheetApp.openById(arquivo.getId());
    }
  }

  const planilha = SpreadsheetApp.create(NOME_PLANILHA_TELEMETRIA);
  DriveApp.getFileById(planilha.getId()).moveTo(pasta);
  propriedades.setProperty(PROPRIEDADE_ID_PLANILHA, planilha.getId());
  return planilha;
}

function obterAbaDaSessao_(planilha, sessao) {
  const nomeAba = 'sessao_' + String(sessao).toLowerCase();
  const existente = planilha.getSheetByName(nomeAba);
  if (existente) {
    return existente;
  }

  const abas = planilha.getSheets();
  if (abas.length === 1 && abas[0].getLastRow() === 0) {
    abas[0].setName(nomeAba);
    return abas[0];
  }
  return planilha.insertSheet(nomeAba);
}

function resposta_(texto) {
  return ContentService.createTextOutput(texto)
    .setMimeType(ContentService.MimeType.TEXT);
}
