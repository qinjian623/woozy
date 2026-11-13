# Woozy 编译器移植测试

`selfhost/src/wzcc/` 是新编译器的实际实现。测试里的 C++ 程序仅作对照，不被新编译器链接或调用。运行时测试用的旧前端命令均替换成会失败并记录调用的 wrapper。

用已构建的 C++ stage0 检查当前 Wz 实现：

```sh
python3 tests/selfhost/front_diff.py --stage0 build/woozy-stdlib-native/woozyc --require-wz
python3 tests/selfhost/smoke.py --stage0 build/woozy-stdlib-native/woozyc
ctest --test-dir build/woozy-stdlib-native -R '^selfhost\.' --output-on-failure
```

`front_diff.py` 读取实时源码，不提供旧副本替代缺失模块。逐字段比较原始/正规化字节、UTF-16 位置、token/trivia/string value、完整 AST、恢复标记、阶段状态和诊断。parse 结果保留一次词法诊断后追加语法诊断；测试对照也采用同一定义。遗漏字段、顺序改变、超时、旧前端调用或模块缺失均不能通过。

检查分开覆盖真实 SourceManager、多种失败分类的预制 token parser、大整数 Python int 对照、TOML typed-value 对照、原始字节 SHA-256、CLI 参数/错误输出和静态 ELF。TOML 的零年日期由现有 toml++ 另行确认，因为 Python datetime 不能表示零年。CRLF 用例以原始字节读取，错误位置按原输入码点检查。

2026-11-13 实际结果：

- SourceManager：O0/O2 检查和 13 个独立 Panic 模式均通过。
- Parser：O0/O2 各 3,484 项通过。
- 完整前端：C++ 对照及 Wz O0/O2 各 121/121 输入一致；后续增加源码时数量随之增加。
- CLI：O0/O2 各 80 个现有语法用例通过；无 ELF INTERP/NEEDED。
- BigInt：14 次构建执行通过；另有 20 项独立运算及输入别名不变检查通过。
- TOML：Python 3.12.15 下 O0/O2 共 2,126 项通过，含当时 964 个项目清单；另有 38 次独立修正检查通过。
- SHA-256：O0/O2 共 34 项摘要及 34 项输入不变检查通过，含百万字节输入。

这些检查尚未覆盖新编译器的类型检查、代码生成和分代自编译。后续必须完成相同源码 stage0→1→2→3，以及现有程序接受/拒绝、诊断、执行和第二/三代一致性检查。
