# Editor integration

The C++ and self-hosted Woozy builds of `woozyc` provide syntax diagnostics
and whole-document formatting. The C++ frontend-only build works without LLVM.
Use [the build guide](BUILD.md) to build `woozyc` and put it on your editor's
`PATH`, or set the server command to its executable path.

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

Add this to `init.lua` to start the built-in LSP client when a `.wz` file
opens. Use `:lua vim.lsp.buf.format({ name = "woozyc" })` to format the buffer.
[Neovim LSP documentation](https://neovim.io/doc/user/lsp/).

```lua
vim.filetype.add({ extension = { wz = "woozy" } })

vim.api.nvim_create_autocmd("FileType", {
  group = vim.api.nvim_create_augroup("woozy-lsp", { clear = true }),
  pattern = "woozy",
  callback = function(args)
    vim.lsp.start({
      name = "woozyc",
      cmd = { "woozyc", "lsp" },
      root_dir = vim.fs.root(args.buf, { "lang.toml", ".git" })
        or vim.fs.dirname(vim.api.nvim_buf_get_name(args.buf)),
    }, { bufnr = args.buf })
  end,
})
```

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

Add this to your Emacs init file. After opening a `.wz` file, run
`M-x eglot` to connect and `M-x eglot-format-buffer` to format.
[Eglot server configuration](https://www.gnu.org/software/emacs/manual/html_node/eglot/Setting-Up-LSP-Servers.html).

```elisp
(define-derived-mode woozy-mode prog-mode "Woozy")
(add-to-list 'auto-mode-alist '("\\.wz\\'" . woozy-mode))

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs '(woozy-mode . ("woozyc" "lsp"))))
```
