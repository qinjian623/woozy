" woozy-editor-setup managed file
function! s:EnableSyntax() abort
  if !exists('g:syntax_on')
    syntax manual
  endif
  setlocal syntax=woozy
endfunction

augroup woozy_filetype
  autocmd!
  autocmd BufRead,BufNewFile *.wz setfiletype woozy
  autocmd FileType woozy call s:EnableSyntax()
augroup END

let s:compiler = @WOOZYC@
if !executable(s:compiler)
  finish
endif

function! s:RootUri(_) abort
  let path = lsp#utils#get_buffer_path()
  let root = lsp#utils#find_nearest_parent_file_directory(path, 'lang.toml')
  return lsp#utils#path_to_uri(empty(root) ? fnamemodify(path, ':h') : root)
endfunction

augroup woozy_lsp
  autocmd!
  autocmd User lsp_setup call lsp#register_server({
        \ 'name': 'woozyc',
        \ 'cmd': {server_info -> [s:compiler, 'lsp']},
        \ 'root_uri': function('s:RootUri'),
        \ 'allowlist': ['woozy']})
  autocmd FileType woozy setlocal omnifunc=lsp#complete
augroup END
