from pathlib import Path
import re
import difflib

source = Path(r'C:\Users\theba\.codex\attachments\6eb13676-2f36-49d0-a5f6-a4b7827f09d3\Texto colado.txt')
original = source.read_text(encoding='utf-8')
old_title = '\\title{\\textit{Análise e Desenvolvimento de um Veículo Elétrico de Pequeno Porte\\\\\n}}'
new_title = '\\title{Análise e Desenvolvimento de um Veículo Elétrico de Pequeno Porte}'
assert original.count(old_title) == 1
formatted = original.replace(old_title, new_title, 1)

start = formatted.index('\\author{')
end = formatted.index('\\maketitle', start)
old_author = formatted[start:end]
new_author = r'''\author{
\IEEEauthorblockN{Gabriel Muttoni, Paulo César Vargas Luz, Adylson Hahn, Érick Ramon Biscaglia}
\IEEEauthorblockA{\textit{Engenharia Elétrica} \\
\textit{Universidade Federal de Santa Maria}\\
\textit{Cachoeira do Sul, Brasil} \\
gabriel.muttoni@acad.ufsm.br\quad paulo.c.luz@ufsm.br\\
adylson.hahn@acad.ufsm.br\quad erick.biscaglia@acad.ufsm.br}
}

'''
formatted = formatted[:start] + new_author + formatted[end:]
old_publication = 'Diário Oficial da União'
new_publication = r'\textit{Diário Oficial da União}'
assert formatted.count(old_publication) == 1
formatted = formatted.replace(old_publication, new_publication, 1)

# Verify the entire document outside the explicitly edited title and author
# block, allowing only the italic wrapper around the publication name.
assert formatted.split(r'\maketitle', 1)[1].replace(new_publication, old_publication) == original.split(r'\maketitle', 1)[1]
assert formatted.split(r'\title', 1)[0] == original.split(r'\title', 1)[0]
for name in re.findall(r'\\IEEEauthorblockN\{([^}]+)\}', old_author):
    assert new_author.count(name) == 1, name
emails = re.findall(r'[\w.]+@[\w.]+', old_author)
assert len(emails) == 4
for email in emails:
    assert new_author.count(email) == 1, email
for affiliation in ['Engenharia Elétrica', 'Universidade Federal de Santa Maria', 'Cachoeira do Sul, Brasil']:
    assert old_author.count(affiliation) == 4
    assert new_author.count(affiliation) == 1
for environment in ['figure', 'equation']:
    pattern = r'\\begin\{' + environment + r'\}.*?\\end\{' + environment + r'\}'
    before = re.findall(pattern, original, re.S)
    after = re.findall(pattern, formatted, re.S)
    assert before == after, environment
    print(f'{environment}: {len(before)} blocos integralmente preservados')

output = Path('output/latex/artigo_formatado_sepoc.tex')
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(formatted, encoding='utf-8', newline='\n')
assert output.read_text(encoding='utf-8') == formatted
Path('tmp/pdfs/alteracoes.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True), formatted.splitlines(True), fromfile='original', tofile='formatado')), encoding='utf-8')
print('Corpo, resumo, palavras-chave, legendas, equações e referências: conteúdo preservado.')
print('Quatro nomes e quatro e-mails preservados; afiliação comum consolidada conforme solicitado.')
print(output.resolve())
