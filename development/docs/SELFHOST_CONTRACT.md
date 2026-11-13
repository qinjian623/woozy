# Woozy 编译器移植接口 selfhost/1

目标是完整替换现有语言前端和两个代码生成器。新编译器的源码位于 `selfhost/src/`；旧 C++ 编译器作为 stage0 和测试对照保留。语言规则、诊断和既有能力限制不变。

Root 单独修改 `selfhost/src/wzcc/model/`、项目清单和 CLI。其他模块导入 `app/wzcc/model`，不各自重新定义 Span、Token、AstNode 或诊断。所有序号沿用现有 C++ 枚举的顺序；数组下标从 0 开始，Source id 从 1 开始。span 是原始文件字节区间；Position 为从 0 开始的 UTF-16 行列。

第一批接口：

- `wzcc/source.snapshot(file_id u64, revision_id u64, id u64, path string, bytes Vec[u8]) model.Source`
- `wzcc/source.raw_span(source model.Source, begin u64, end u64) model.Span`
- `wzcc/source.span(source model.Source, begin usize, end usize) model.Span`：输入正规化字节位置，输出原始字节位置。
- `wzcc/source.position(source model.Source, byte u64) model.Position`
- `wzcc/source.byte_at(source model.Source, position model.Position) u64`
- `wzcc/lexer.lex(source model.Source) model.LexResult`
- `wzcc/parser.parse(input model.LexResult) model.ParseResult`

Source 保留原始字节、正规化字节与字节对应表；只移除文件开头 BOM，并将 CRLF 转成 LF。UTF-8、原始 NUL 和孤立 CR 的失败位置与现有 SourceSnapshot 相同。boundaries 只保存 Unicode 字符边界及其 UTF-16 位置；不得把字节数当列号。span、position、byte_at 的无效内部调用是实现错误，可 Panic；不得把无效位置静默取整。

Token 与 trivia 的 spelling 是正规化文本对应的 UTF-8 内容，span 保持原始字节区间。字符串解码值使用 has_string_value 区分空字符串与不存在。AST 使用连续 nodes 与 children 下标；不持有递归对象引用。

LexResult/ParseResult 的 has_value 对应现有 PhaseResult.value。诊断顺序、恢复节点、token 区间、子节点顺序与现有实现一致。parser.parse 负责保留词法诊断；不在内部重跑词法器或重复追加诊断。

首批阶段只发布显式词法/语法操作；未移植的名称/类型检查及 build 必须明确报 E_CAPABILITY_UNAVAILABLE。语法成功不得被报告为完整编译成功。

LLVM/Clang/LLD 与现有运行库可以保留。新编译器不得启动 woozyc/lang_native_helper 或通过 FFI 调用旧前端。测试对照程序可使用 stage0 库，但不能链接进或被新编译器调用。

后续按模块移植清单/绑定/解析、常量、类型与泛型、所有权/HIR、Native 控制流与文本 LLVM IR、Python 发射器。完整完成条件为同份源码 stage0→1→2→3，接受/拒绝、诊断与程序执行对照通过；阶段2/3行为一致。不得以只支持表达式、忽略复杂语法、套壳旧编译器或只比较程序退出0来代替。

模块逻辑导入名以 `app/wzcc/` 开头（现有加载器以 app 表示当前 source_root）。枚举 ID 通过 model.token_kind_id/trivia_kind_id/ast_kind_id 输出，顺序与 C++ 一致。LexResult/ParseResult 同时保留 state 和 failure。前端阶段另安排取消测试，不得把 cancelled 报成成功。

## SourceManager（SH01b）

Source包追加公开 `Manager { pub files Vec[string]; pub snapshots Vec[model.Source]; pub revision u64; }` 和 `manager() Manager`、`add_file(state inout Manager, path string) u64`、`update(state inout Manager, file_id u64, bytes Vec[u8]) model.Source`、`get(state Manager, snapshot_id u64) model.Source`。严格沿用CPP SourceManager的1起始file/snapshotID与全局递增revision；update保留旧快照，非法source仍记录快照。空path/非法ID是内部错误，不能静默创建别的file。已有snapshot六函数与共享model不变。
