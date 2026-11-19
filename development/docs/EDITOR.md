# Editor integration

## Install the editor configuration

On Linux, run the [setup tool](../editor/setup.py) from the repository root
with Python 3, Neovim 0.10 or later, Emacs with Eglot (included in Emacs 29
or later), and a working `woozyc` executable. See the [compiler build guide](BUILD.md),
[Neovim LSP documentation](https://neovim.io/doc/user/lsp/), and
[Eglot manual](https://www.gnu.org/software/emacs/manual/html_mono/eglot.html).

```sh
python3 development/editor/setup.py
```

The tool installs Woozy syntax highlighting and automatic LSP startup for
both editors. It uses the `woozyc` found on `PATH` at setup time and writes its
absolute path into the editor settings. Pass `--compiler` to select another
executable, including a build outside `PATH`.

```sh
python3 development/editor/setup.py --editor neovim
python3 development/editor/setup.py --editor emacs --compiler /path/to/woozyc
```

Neovim files go in the user config directory selected by `XDG_CONFIG_HOME`
and `NVIM_APPNAME`. Emacs loads the installed mode and Eglot settings from a
marked block in its init file. Use `--emacs-init FILE` for a custom init.
Running setup again updates the managed files and block. Setup stops before
changing files if it finds a conflicting Woozy file that it does not manage.
The tool uses local files and the selected compiler executable.

Restart your editor and open a `.wz` file. Files under the same
`lang.toml` use the same project root for LSP.

## Language server

The C++ and self-hosted Woozy builds of `woozyc` provide syntax diagnostics
and whole-document formatting. The C++ frontend-only build works without LLVM.

Run `woozyc lsp` to start the server on standard input and output.
Use the self-hosted build for completion, type information on hover,
go-to-definition, and live project diagnostics.

The server finds the nearest `lang.toml` above an open file. It uses unsaved
text for open documents and reads the remaining project files from disk.
An editor session never writes buffer contents to disk. Project checks run
in a background thread, and checks for superseded revisions are discarded.
Completion uses the current lexical scope, package exports, and members
whose receiver type can be resolved. Incomplete code can leave some types
unresolved. Hover and definition lookup use the current analysis.
The self-hosted server and formatter run without the C++ compiler, Python,
or LLVM. Project checks are also available from the CLI.

## Command line

```sh
woozyc check --syntax-only examples/core.wz
woozyc check --syntax-only --format json examples/core.wz
woozyc check --project lang.toml
woozyc fmt examples/core.wz
woozyc fmt --check examples/core.wz
cat examples/core.wz | woozyc fmt -
```

`fmt` uses four-space indentation, writes formatted source to stdout, and
leaves the file on disk intact.
`fmt --check` returns 0 when the source is already formatted and 1 when
formatting would change it. Invalid syntax returns 1 with a diagnostic on
stderr and no formatted output. Use `fmt -` to format text from stdin.

## Neovim

Use `:lua vim.lsp.buf.format({ name = "woozyc" })` to format the buffer.
[Neovim LSP documentation](https://neovim.io/doc/user/lsp/).

## Vim

If you use the [vim-lsp plugin](https://github.com/prabirshrestha/vim-lsp),
add this to `.vimrc` and use `:LspDocumentFormat` to format a Woozy buffer.

```vim
augroup woozy_filetype
  autocmd!
  autocmd BufNewFile,BufRead *.wz setfiletype woozy
  autocmd User lsp_setup call lsp#register_server({
        \ 'name': 'woozyc',
        \ 'cmd': {server_info -> ['woozyc', 'lsp']},
        \ 'allowlist': ['woozy'],
        \ })
augroup END
```

## Emacs

Use `M-x eglot-format-buffer` to format the buffer.
[Eglot server configuration](https://www.gnu.org/software/emacs/manual/html_node/eglot/Setting-Up-LSP-Servers.html).
