from pathlib import Path
import re
import difflib

source = Path('output/latex/artigo_formatado_sepoc.tex')
before = source.read_text(encoding='utf-8')
after, count = re.subn(r'\\subsection\{([^{}]+)\}', lambda m: r'\subsection{\textit{' + m.group(1) + '}}', before)
assert count == 11

old_abstract = '\\begin{abstract}\n\\bfseries\n'
new_abstract = '\\begin{abstract}\n\\bfseries\\itshape\n'
assert after.count(old_abstract) == 1
after = after.replace(old_abstract, new_abstract, 1)

# Keep the existing language-dependent label, changing only its typography.
label_format = '\\let\\SEPOCabstractname\\abstractname\n\\renewcommand{\\abstractname}{\\textbf{\\textit{\\SEPOCabstractname}}}\n'
after = after.replace(r'\begin{abstract}', label_format + r'\begin{abstract}', 1)

old_keywords = '\\renewcommand{\\IEEEkeywordsname}{Palavras-chave}\n\\begin{IEEEkeywords}\n'
new_keywords = '\\renewcommand{\\IEEEkeywordsname}{\\textbf{\\textit{Palavras-chave}}}\n\\begin{IEEEkeywords}\n\\bfseries\n'
assert after.count(old_keywords) == 1
after = after.replace(old_keywords, new_keywords, 1)

# Remove exactly the new formatting commands to prove full source preservation.
restored = re.sub(r'\\subsection\{\\textit\{([^{}]+)\}\}', lambda m: r'\subsection{' + m.group(1) + '}', after)
restored = restored.replace(label_format, '', 1)
restored = restored.replace(new_abstract, old_abstract, 1)
restored = restored.replace(new_keywords, old_keywords, 1)
assert restored == before, 'Unexpected modification outside formatting'
for environment in ['figure', 'equation']:
    pattern = r'\\begin\{' + environment + r'\}.*?\\end\{' + environment + r'\}'
    old_blocks = re.findall(pattern, before, re.S)
    assert old_blocks == re.findall(pattern, after, re.S)
    print(f'{environment}: {len(old_blocks)} blocos inalterados')

output = Path('output/latex/artigo_formatado_sepoc_v2.tex')
output.write_text(after, encoding='utf-8', newline='\n')
assert output.read_text(encoding='utf-8') == after
Path('tmp/pdfs/alteracoes_v2.diff').write_text(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile='v1', tofile='v2')), encoding='utf-8')
print(f'{count} subtítulos com itálico explícito; resumo em negrito e itálico.')
print('Verificação integral: somente comandos de formatação foram modificados.')
print(output.resolve())
