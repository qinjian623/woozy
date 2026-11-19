#!/usr/bin/env python3
"""Install the repository's Woozy editor configuration for the current user."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import unicodedata


ASSETS = Path(__file__).resolve().parent
OWNER = b'woozy-editor-setup'
COMPILER_TOKEN = b'@WOOZYC@'
BEGIN = b';; BEGIN Woozy editor setup'
END = b';; END Woozy editor setup'
MARKERS = re.compile(rb'^(' + BEGIN + b'|' + END + rb')(?:\r?\n|\Z)', re.MULTILINE)


def quote(path, *, elisp=False):
    value = str(path)
    if any(unicodedata.category(char) in {'Cc', 'Cs'} for char in value):
        raise ValueError(f'Unsupported character in path {value!r}')
    literal = json.dumps(value, ensure_ascii=False)
    if elisp:
        # An existing Emacs init file can use a coding system other than UTF-8.
        literal = ''.join(
            char if char.isascii() else f'\\U{ord(char):08x}'
            for char in literal
        )
    return literal.encode('utf-8')


def compiler_path(option):
    location = option if option is not None else shutil.which('woozyc')
    if location is None:
        raise ValueError('woozyc was not found on PATH. Use --compiler PATH.')
    path = Path(location).expanduser().absolute()
    quote(path)
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f'Compiler is not an executable file: {path}')
    try:
        version = subprocess.run([path, '--version'], capture_output=True, timeout=5)
    except subprocess.TimeoutExpired as error:
        raise ValueError(f'Timed out running {path} --version') from error
    if version.returncode != 0 or not re.match(rb'woozyc [0-9]', version.stdout):
        raise ValueError(f'{path} --version did not report a Woozy compiler')
    return path


def emacs_paths(option, home, xdg):
    legacy_dir = home / '.emacs.d'
    legacy = (home / '.emacs.el', home / '.emacs')
    xdg_dir = xdg / 'emacs'
    if option is not None:
        init = Path(option).expanduser().absolute()
    elif xdg_dir.is_dir() and not legacy_dir.is_dir() and not legacy[1].exists():
        init = xdg_dir / 'init.el'
    else:
        init = next((path for path in legacy if path.exists() or path.is_symlink()),
                    legacy_dir / 'init.el')
    target = init.resolve(strict=init.is_symlink())
    if init.suffix == '.elc' or target.suffix == '.elc':
        raise ValueError(f'Expected an Emacs source init file: {init}')
    if option is None:
        compiled = init.with_suffix('.elc')
        if compiled.exists() or (init.parent == legacy_dir and (home / '.emacs.elc').exists()):
            raise ValueError('A compiled Emacs init takes precedence. Configure its source with --emacs-init and recompile it.')
    lisp = (legacy_dir if init in legacy else init.parent) / 'lisp'
    return target, lisp


def managed_init(content, path, lisp):
    markers = list(MARKERS.finditer(content))
    if markers and [match[1] for match in markers] != [BEGIN, END]:
        raise ValueError(f'{path} contains incomplete or duplicate Woozy setup markers')
    newline_source = markers[0][0] if markers else content
    newline = b'\r\n' if newline_source.partition(b'\n')[0].endswith(b'\r') else b'\n'
    load = b'(load ' + quote(lisp / 'woozy-lsp.el', elisp=True) + b" nil 'nomessage)"
    block = newline.join((BEGIN, load, END, b''))
    if markers:
        return content[:markers[0].start()] + block + content[markers[1].end():]
    separator = newline if content and not content.endswith(b'\n') else b''
    return content + separator + block


def read_file(path):
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return None, None
    if not stat.S_ISREG(mode):
        raise ValueError(f'Refusing to replace a non-regular file: {path}')
    return path.read_bytes(), stat.S_IMODE(mode)


def install_file(path, contents, previous, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.woozy-', delete=False) as file:
        temp = Path(file.name)
        try:
            file.write(contents)
            file.close()
            if mode is not None:
                temp.chmod(mode)
            if read_file(path) != (previous, mode):
                raise ValueError(f'File changed during setup: {path}')
            temp.replace(path)
        finally:
            temp.unlink(missing_ok=True)


def install(args):
    compiler = compiler_path(args.compiler)
    home = Path.home().absolute()
    xdg = Path(os.environ.get('XDG_CONFIG_HOME') or home / '.config').absolute()
    assets = []
    init = None
    if args.editor in {'both', 'neovim'}:
        app = Path(os.environ.get('NVIM_APPNAME') or 'nvim')
        if app.is_absolute() or not app.parts or '..' in app.parts:
            raise ValueError('NVIM_APPNAME must be a relative name without a parent component')
        quote(xdg / app)
        assets.extend((ASSETS / 'nvim' / name, xdg / app / name, replacement) for name, replacement in (
            ('plugin/woozy.lua', quote(compiler)), ('syntax/woozy.vim', None),
        ))
    if args.editor in {'both', 'emacs'}:
        init, lisp = emacs_paths(args.emacs_init, home, xdg)
        quote(init)
        assets.extend((ASSETS / 'emacs' / name, lisp / name, replacement) for name, replacement in (
            ('woozy-mode.el', None), ('woozy-lsp.el', quote(compiler, elisp=True)),
        ))

    # Check every destination before creating directories or writing files.
    changes = []
    paths = {init} if init is not None else set()
    for source, target, replacement in assets:
        target = target.parent.resolve() / target.name
        if target in paths:
            raise ValueError(f'Conflicting setup destinations: {target}')
        paths.add(target)
        contents = source.read_bytes()
        if OWNER not in contents.split(b'\n', 1)[0]:
            raise ValueError(f'Missing setup marker in {source}')
        if replacement is not None:
            if contents.count(COMPILER_TOKEN) != 1:
                raise ValueError(f'Invalid compiler placeholder in {source}')
            contents = contents.replace(COMPILER_TOKEN, replacement)
        previous, mode = read_file(target)
        if previous == contents:
            continue
        if previous is not None and OWNER not in previous.split(b'\n', 1)[0]:
            raise ValueError(f'Existing editor file is not managed by Woozy setup: {target}')
        changes.append((target, contents, previous, mode))

    if init is not None:
        previous, mode = read_file(init)
        if previous is not None and b'\0' in previous:
            raise ValueError(f'Expected an Emacs source init file: {init}')
        contents = managed_init(previous or b'', init, lisp)
        if contents != previous:
            changes.append((init, contents, previous, mode))
    for path in paths:
        if any(parent in paths for parent in path.parents):
            raise ValueError(f'Conflicting setup destinations: {path}')

    for path, contents, previous, mode in changes:
        install_file(path, contents, previous, mode)
        print(f'Updated {path}')
    if not changes:
        print('Woozy editor setup is up to date.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', choices=('both', 'neovim', 'emacs'), default='both')
    parser.add_argument('--compiler', metavar='PATH', help='Woozy executable (default: woozyc on PATH)')
    parser.add_argument('--emacs-init', metavar='FILE', help='Emacs init file (default: detect the usual locations)')
    try:
        install(parser.parse_args())
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f'Woozy editor setup: {error}\n')


if __name__ == '__main__':
    main()
