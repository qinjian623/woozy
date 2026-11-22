"""Install a pinned vim-lsp in Vim's native package directory."""

from hashlib import sha256
from io import BytesIO
import os
from pathlib import Path, PurePosixPath
import shutil
import tarfile
from tempfile import TemporaryDirectory
from urllib.request import urlopen


VIM_LSP_COMMIT = '977e704cdbbbbc89a54f8f226d3468b0aa0405c9'
VIM_LSP_URL = f'https://codeload.github.com/prabirshrestha/vim-lsp/tar.gz/{VIM_LSP_COMMIT}'
VIM_LSP_SHA256 = '6cbbdf6cab8a1241cbe3eb4068475333ac17200d3c40d5dacf599f2eaf54bbce'
VIM_LSP_ARCHIVE_SIZE = 155507
VIM_LSP_FILES = {'autoload', 'doc', 'ftplugin', 'plugin', 'syntax', 'LICENSE', 'LICENSE-THIRD-PARTY'}


def is_vim_lsp(path):
    return (path / 'plugin/lsp.vim').is_file() and (path / 'autoload/lsp.vim').is_file()


def unpack_vim_lsp(archive, staging):
    with tarfile.open(fileobj=BytesIO(archive), mode='r:gz') as package:
        for member in package:
            parts = PurePosixPath(member.name).parts
            if not parts or parts[0] != f'vim-lsp-{VIM_LSP_COMMIT}' or '..' in parts:
                raise ValueError(f'Unexpected path in vim-lsp archive: {member.name!r}')
            if len(parts) == 1 or parts[1] not in VIM_LSP_FILES:
                continue
            output = staging.joinpath(*parts[1:])
            if member.isdir():
                output.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                output.parent.mkdir(parents=True, exist_ok=True)
                with package.extractfile(member) as source, output.open('xb') as target:
                    shutil.copyfileobj(source, target)
            else:
                raise ValueError(f'Unexpected vim-lsp archive member: {member.name!r}')
    if not is_vim_lsp(staging):
        raise ValueError('vim-lsp archive is missing its Vim plugin files')
    (staging / 'woozy-upstream-commit').write_text(f'{VIM_LSP_COMMIT}\n', encoding='ascii')


def install_vim_lsp(vim_home: Path) -> bool:
    """Return True after installing, or False when a start package already exists."""
    packages = vim_home / 'pack'
    if any(is_vim_lsp(path) for path in packages.glob('*/start/*')):
        return False
    target = packages / 'woozy/start/vim-lsp'
    if os.path.lexists(target):
        raise ValueError(f'Existing vim-lsp path is not a usable start package: {target}')

    with urlopen(VIM_LSP_URL, timeout=30) as response:
        archive = response.read(VIM_LSP_ARCHIVE_SIZE + 1)
    if sha256(archive).hexdigest() != VIM_LSP_SHA256:
        raise ValueError('vim-lsp archive failed SHA-256 verification')

    target.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='.vim-lsp-', dir=target.parent) as temp:
        staging = Path(temp) / 'vim-lsp'
        unpack_vim_lsp(archive, staging)
        if is_vim_lsp(target):
            return False
        if os.path.lexists(target):
            raise ValueError(f'vim-lsp path appeared during setup: {target}')
        try:
            staging.rename(target)
        except OSError:
            if is_vim_lsp(target):
                return False
            raise
    return True
