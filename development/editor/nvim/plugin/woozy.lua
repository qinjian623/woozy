-- woozy-editor-setup managed file
vim.filetype.add({ extension = { wz = "woozy" } })

vim.api.nvim_create_autocmd("FileType", {
  group = vim.api.nvim_create_augroup("woozy_lsp", { clear = true }),
  pattern = "woozy",
  callback = function(args)
    local filename = vim.api.nvim_buf_get_name(args.buf)
    vim.lsp.start({
      name = "woozyc",
      cmd = { @WOOZYC@, "lsp" },
      root_dir = vim.fs.root(args.buf, "lang.toml") or vim.fs.dirname(filename) or vim.uv.cwd(),
    }, { bufnr = args.buf })
  end,
})
