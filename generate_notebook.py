"""Build apex_complete_analysis.ipynb from notebook_source.py.

notebook_source.py is the single source of the notebook. Cells start at a
"# %%" line; "# %% [markdown]" cells have each line prefixed with "# ".
Anything before the first marker is a file header and is skipped.

Usage:
    python3 generate_notebook.py            # write the notebook
    python3 generate_notebook.py --execute  # write it, then run it in place
"""
import subprocess
import sys
from pathlib import Path

import nbformat as nbf

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / 'notebook_source.py'
NOTEBOOK = ROOT / 'apex_complete_analysis.ipynb'


def split_cells(text):
    cells, kind, buf = [], None, []

    def flush():
        if kind is None:
            return
        body = '\n'.join(buf).strip('\n')
        if body:
            cells.append((kind, body))

    for line in text.splitlines():
        if line.startswith('# %%'):
            flush()
            kind, buf = ('markdown' if '[markdown]' in line else 'code'), []
            continue
        if kind == 'markdown':
            if line.startswith('# '):
                line = line[2:]
            elif line.strip() == '#':
                line = ''
        buf.append(line)
    flush()
    return cells


def build():
    nb = nbf.v4.new_notebook()
    nb.metadata = {
        'kernelspec': {'display_name': 'Python 3', 'language': 'python', 'name': 'python3'},
        'language_info': {'name': 'python'},
    }
    for kind, body in split_cells(SOURCE.read_text(encoding='utf-8')):
        nb.cells.append(nbf.v4.new_markdown_cell(body) if kind == 'markdown' else nbf.v4.new_code_cell(body))
    nbf.write(nb, NOTEBOOK)
    print(f'Wrote {NOTEBOOK.name} ({len(nb.cells)} cells) from {SOURCE.name}')


if __name__ == '__main__':
    build()
    if '--execute' in sys.argv[1:]:
        subprocess.run([sys.executable, '-m', 'nbconvert', '--to', 'notebook', '--execute', '--inplace',
                        '--ExecutePreprocessor.timeout=600', str(NOTEBOOK)], cwd=ROOT, check=True)
        print(f'Executed {NOTEBOOK.name}')
