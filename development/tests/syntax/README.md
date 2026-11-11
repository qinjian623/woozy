# 语法用例数据（T02 预备）

本目录包含固定语法输入、C++ lexer/parser 测试和真实 CLI 分类检查。数据文件：[cases.json](../../../tests/syntax/cases.json)；生成器：[export_syntax_cases.py](../../../tools/export_syntax_cases.py)。

## 来源

- `in_memory_cases`：59 个。生成器用 `runpy` 加载 [check_syntax.py](../../../baseline/tools/check_syntax.py)，以 `sys.settrace` 在 `self_test` 返回时读取局部 `cases`；名称、完整源文本和布尔期望均未手抄。布尔值转为 `expected_syntax` 的 `accept`/`reject`。
- `file_cases`：21 个。16 个来自 [validation.json](../../../baseline/validation.json) 和 [examples_notes.json](../../../baseline/examples_notes.json)，路径为 `baseline/samples/...`；7 正例、9 负例，负例保留 `rejection_phase`。5 个来自 [test-cases.json](../../../contracts/test-cases.json)，路径为 `examples/...`，保留 `expected_semantic`。
- 4 个基线语义负例和 `use_after_move` 必须语法接受；语义拒绝留给后续阶段。

## 字段

`in_memory_cases[]`：`name`、`source`、`expected_syntax`。
`file_cases[]`：`name`、`path`、`source`、`expected_syntax`；基线负例另有 `rejection_phase`，合同样例另有 `expected_semantic`。

完整源文本保留 CRLF、BOM、中文和转义；不复制实际诊断、token 数或执行期望。21 个文件是独立模块，不能拼接。

## 生成与自检

```sh
python3 tools/export_syntax_cases.py
python3 tools/export_syntax_cases.py --check
```

脚本每次独立加载原型、捕获 `self_test`、构造导出，并执行两次后比较完整字节。它还检查：原型 59/59、59 个内存用例和 16+5=21 个文件、名称/路径唯一、每项用原 `check_source`/`check_file` 对照、25 个基线文件 SHA-256 不变。`--check` 只比较不写文件。

辅助命令：

```sh
python3 baseline/tools/check_syntax.py --self-test
python3 -m json.tool tests/syntax/cases.json >/dev/null
git diff --check
```

## 限制

`59/59` 是 Python 语法原型的自测结果。C++ 前端另外通过了 Debug/Release 各 80/80（59 个内存输入和 21 个实际文件）的分类及 JSON schema 检查，并已接入 CTest。单独执行方法为 python3 tests/syntax/run_cases.py build/frontend-debug/woozyc。

这些结果不证明类型、效果、所有权、代码生成或运行时正确。语义负例通过语法阶段是预期行为。
