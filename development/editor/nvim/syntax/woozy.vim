" woozy-editor-setup managed file
if exists('b:current_syntax')
  finish
endif

syntax case match
syntax iskeyword 48-57,65-90,95,97-122

syntax keyword woozyInclude package import
syntax keyword woozyStructure enum impl struct trait
syntax keyword woozyStorageClass const inout let move pub var
syntax keyword woozyConditional case default else if switch
syntax keyword woozyRepeat for in
syntax keyword woozyException catch throw throws try
syntax keyword woozyStatement break continue defer return
syntax keyword woozyKeyword as async await func unsafe
syntax keyword woozyBoolean false true
syntax keyword woozyReserved class extends extern operator

syntax keyword woozyType bool i8 i16 i32 i64 u8 u16 u32 u64 isize usize f32 f64 string Unit
syntax keyword woozyType Self Vec Slice Weak Task Option Result

syntax keyword woozyTodo TODO FIXME XXX NOTE contained
syntax match woozyComment "//.*" contains=woozyTodo
syntax region woozyComment start="/\*" end="\*/" contains=woozyTodo

syntax match woozyEscape +\\\%([\\"nrt0]\|u{[0-9A-Fa-f]\{1,6}}\)+ contained
syntax region woozyString start=+"+ skip=+\\[\\"]+ end=+"+ end=+$+ oneline contains=woozyEscape
syntax region woozyRawString start="`" end="`"

syntax match woozyNumber "\<0b[01]\%(_\?[01]\)*\>"
syntax match woozyNumber "\<0o[0-7]\%(_\?[0-7]\)*\>"
syntax match woozyNumber "\<0x[0-9A-Fa-f]\%(_\?[0-9A-Fa-f]\)*\>"
syntax match woozyNumber "\<\%(0\|[1-9]\%(_\?\d\)*\)\>"
syntax match woozyFloat "\<\%(0\|[1-9]\%(_\?\d\)*\)\%(\.\d\%(_\?\d\)*\%([eE][+-]\?\d\%(_\?\d\)*\)\?\|[eE][+-]\?\d\%(_\?\d\)*\)\>"

syntax sync fromstart

highlight default link woozyInclude Include
highlight default link woozyStructure Structure
highlight default link woozyStorageClass StorageClass
highlight default link woozyConditional Conditional
highlight default link woozyRepeat Repeat
highlight default link woozyException Exception
highlight default link woozyStatement Statement
highlight default link woozyKeyword Keyword
highlight default link woozyBoolean Boolean
highlight default link woozyReserved Error
highlight default link woozyType Type
highlight default link woozyTodo Todo
highlight default link woozyComment Comment
highlight default link woozyEscape SpecialChar
highlight default link woozyString String
highlight default link woozyRawString String
highlight default link woozyNumber Number
highlight default link woozyFloat Float

let b:current_syntax = 'woozy'
