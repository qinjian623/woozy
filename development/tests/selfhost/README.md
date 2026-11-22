# Woozy 编译器移植测试

`selfhost/src/wzcc/` 是新编译器的实际实现。测试里的 C++ 程序仅作对照，不被新编译器链接或调用。运行时测试用的旧前端命令均替换成会失败并记录调用的 wrapper。

用已构建的 C++ stage0 检查当前 Wz 实现：

```sh
python3 tests/selfhost/front_diff.py --stage0 build/woozy-stdlib-native/woozyc --require-wz
python3 tests/selfhost/smoke.py --stage0 build/woozy-stdlib-native/woozyc
ctest --test-dir build/woozy-stdlib-native -R '^selfhost\.' --output-on-failure
```

`front_diff.py` 读取实时源码，不提供旧副本替代缺失模块。逐字段比较原始/正规化字节、UTF-16 位置、token/trivia/string value、完整 AST、恢复标记、阶段状态和诊断。parse 结果保留一次词法诊断后追加语法诊断；测试对照也采用同一定义。遗漏字段、顺序改变、超时、旧前端调用或模块缺失均不能通过。

测试还分别覆盖真实 SourceManager、含多种失败分类的预制 token 流 parser 用例、大整数与 Python int 对照、TOML typed-value 对照、原始字节 SHA-256、CLI 参数与错误输出以及静态 ELF。由于 Python datetime 不支持零年，TOML 的零年日期由现有 toml++ 另行核对。CRLF 用例按原始字节读取，错误位置按原始输入码点校验。

2026-11-13 实际结果：

- SourceManager：O0/O2 检查和 13 个独立 Panic 模式均通过。
- Parser：O0/O2 各 3,484 项通过。
- 完整前端：基础模块整合后，C++ 对照及 Wz O0/O2 各 169/169 输入一致；后续增加源码时数量随之增加。
- CLI：O0/O2 各 80 个现有语法用例通过；无 ELF INTERP/NEEDED。
- BigInt：14 次构建执行通过；另有 20 项独立运算及输入别名不变检查通过。
- TOML：Python 3.12.15 下 O0/O2 共 2,126 项通过，含当时 964 个项目清单；另有 38 次独立修正检查通过。
- SHA-256：O0/O2 共 34 项摘要及 34 项输入不变检查通过，含百万字节输入。
- 输入/路径：O0/O2 共 148/148 和 104/104 项通过；路径另有 20 个真实文件系统用例与 C++ 在两种优化下匹配。测试临时项目链接当前源码，仓库不保留生产模块副本。
- 系统操作表：213 项操作、365 个参数及 29 个内建类型与当前 C++ 头文件一致；operation 是枚举值，不能用表下标代替。
- JSON：238 项在 C++ json-c、Wz O0/O2 下匹配；包括既有 json-c 对根 null、终止空白、非有限数、重复键和深度的规则。3 个非法 Source 输入不在调用前提内，未计入 JSON 测试通过数。
- 项目清单：O0/O2 各 1,108 项通过；另有 15 项 C++ 对照检查。保留 schema_version 转换、递归数值范围、数字长度和时间精度限制。TOML/系统错误的部分说明文字仍有差异，分类和位置已对照。
- 数值计算：740 项在 O0/O2 下通过，覆盖大整数、有理数和精确 IEEE 舍入；独立新增 22 项在两种优化下通过。测试用的 C++ 数值 oracle 需要 GMP/MPFR；新编译器不链接它们。
- 类型表：180 行完整类型操作与 25 行位宽错误 trace 在 O0/O2 下匹配 C++；另有 25 行独立检查通过。
- 名称解析：O0/O2 各 197 项、8 组完整字段对照通过；独立新增 11 组 Source 对照通过，含 3000 个常量的环。
- HIR 数据定义：86 行 C++ 对照和 12 组独立非零字段检查在 O0/O2 下通过；87 个数据记录、16 组枚举/variant 的字段和顺序一致。普通 builder 复制仍共享 Vec，不能当作经过验证的独立快照。

这些检查尚未覆盖新编译器的类型检查、代码生成和分代自编译。后续必须完成相同源码 stage0→1→2→3，以及现有程序接受/拒绝、诊断、执行和第二/三代一致性检查。
