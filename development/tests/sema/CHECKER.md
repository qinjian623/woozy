# T04 标量检查器交付

operation_id：`language-t04-scalar-checker-20261103`。契约版本：`compiler-dev/1`。
基础提交：`c53298a`；独立案例取自主目录 `c4c3724`；公共 HIR 取自主目录
`7c49989`。本次不修改任务清单，不提交或推送，不生成代码、不执行源程序。

## API 和数据

`include/lang/sema/check.hpp` 发布 `lang::sema::check_project`：

```cpp
PhaseResult<hir::VerifiedHirModule> check_project(
    std::shared_ptr<const modules::ResolvedProject>,
    std::optional<CheckConfig> = {}, std::stop_token = {});
PhaseResult<hir::VerifiedHirModule> check_project(
    const PhaseResult<modules::ResolvedProject>&,
    std::optional<CheckConfig> = {}, std::stop_token = {});
```

推荐使用第二个重载：名称阶段有失败、恢复或取消状态时，不使用其保留的 value 开始检查。
第一个重载要求调用方已经确认 resolve 成功。空输入或配置不一致返回 internal；
源码错误返回 source，暂未实施的合法功能返回 unsupported；取消无 value、退出码 130。
全部检查诊断使用共享 schema 的 `phase=type`，按 code 区分未支持项。

省略配置时使用 Project 的 backend/word_bits。提供配置时必须与 Project 相同，
检查器不会修改 Project 来冒充另一个目标。native/python 都支持 32/64 位配置。
结果是一个包含项目所有已检查标量函数的公共 HirModule，identity 为 Project.entry；
每个函数、节点和作用域仍保存自己的 ModuleId/file/AstNodeId/SourceSpan。

名称引用使用真实 occurrences，声明使用 symbols.definition。参数及函数体共用根作用域，
绑定初始化后才可见；嵌套块、循环和循环体分别保存 ScopeId。没有重新按字符串搜索绑定，
没有复制 TypeContext 或 HIR 数据定义。TypeId 的 0 无效；其它共享 ID 的 0 正常使用。

成功结果必须经过唯一 `hir::verify`，检查器不能构造 VerifiedHirModule。
发布后的数据由 verifier 深复制，保留原始源码快照。整数节点保存规范十进制数学值和
目标位模式，Source/ResolvedProject 中的源码与声明保留原始拼写；不先经过 host int64。
顶层 const 在检查阶段求值，使用时生成有确定类型的常量；即使没有任何函数，也检查
全部顶层 const。没有顶层运行时初始化。

## 已支持

- bool、全部定宽/目标整数、Unit 和函数签名；局部推断、let/var/const、读写、复合赋值。
- 十进制/十六进制/二进制/八进制整数、分隔下划线、带符号字面量的上下文范围检查，
  最小负数、完整 u64 最大值，以及远大于 u64 的错误输入。无上下文默认 i64；已定类型
  变量不能隐式混宽或混符号。显式数值转换支持 integer->integer 的动态/常量范围检查、
  float->integer（先向零截断，再检查截断后的数学整数范围；已知 NaN/无穷/越界为源码
  错误）和 integer->float（直接按目标精度一次 RNE）。float->float 支持全部 f32/f64
  源目标对；f64->f32 按 IEEE RNE 舍入，有限溢出为带符号 Infinity，不套用浮点到整数
  的失败规则。bool 源/目标仍拒绝。
  const 求值接受整数、bool、已声明函数值和已舍入的浮点常量（含符号零、subnormal、
  NaN/无穷），浮点位值在括号、正负号、已知 `+ - * /` 和引用处不丢失。
- GMP/GMPXX 6.3.0 计算常量。定宽加减乘、负号、位运算和左移回绕；有符号除法向零，
  MIN/-1 回到 MIN，MIN%-1 为 0，负余数随被除数。静态折叠值用于 const 和转换检查；
  普通运行时 Binary/Unary/IntegerConvert 节点保留，不做优化器。
- If/else-if/For/Return/Break/Continue；三段式、条件式和无限循环。跳转保存最近 For 的
  HirNodeId，非 Unit 的正常出口需要 return。字面量 bool 条件选择实际分支；无限循环
  中不可达的 break 不算正常出口，内层 break 不会退出外层循环。其它条件保守分析，
  不以运行时表达式数值或必然 Panic 证明不返回。
- 直接函数调用、函数声明作为值、函数类型参数/返回值/局部变量、返回函数值后立即调用，
  同包跨文件和跨源模块调用、相互递归。直接调用接受命名实参；函数值不保留 label。
  args/children 保留源码顺序，parameter_index 单独记录匹配位置，callee 只出现一次。
- 数值/bool 的 var 整绑定 inout，以及被调函数内的参数读写和继续传入 inout。
  Place 不能作为普通值使用；同 call 的重复 inout、其它实参树中对同存储的读取/借用、
  let/const/普通参数、模式或类型不符均被拒绝。不实施拥有对象或复杂别名分析。
- f32/f64 的类型、参数/返回/绑定/普通传递，以及正十进制字面量直接舍入到目标 IEEE 位。
  字面量舍入使用 GMP 有理数计算，包含 ties-to-even、subnormal、溢出和零的测试。
  这不表示支持完整严格浮点运算。

动态除零、非法 shift 和动态转换溢出保留对应 HIR 操作，由后端实现 Panic。
本任务没有运行时或代码生成验证。

## 明确拒绝及剩余接口问题

已确定的类型、返回、条件和不可写赋值错误使用 `E_TYPE_MISMATCH`；漏参、未知/重复 label、
函数值的 label、模式不匹配、不可写 inout 和同 call 冲突使用 `E_CALL_ARGUMENT`。

`E_CAPABILITY_UNAVAILABLE` 拒绝：泛型/聚合/trait/NeutralBracket/列表（T07）、
throws/throw/try/defer（T08）、Vec/string/ARC/move/复杂 Place（T09）、
Python/native 外部声明和调用（T10/T11）、switch/for-in/unsafe、严格浮点算术、
比较、浮点加减乘除复合赋值和浮点转换。不会因项目包含正常 scalar 操作就统一拒绝。
普通参数重新绑定按尚未确定的规则单独报告未支持；const 始终不可重新赋值。

无捕获函数字面量已支持立即调用、绑定、实参、返回和嵌套。resolver 为每个字面量发布
不进入词法名字表的独立 function SymbolId，checker 生成对应 HirFunction 和 FunctionValue；
verifier 接受真实 Function 或 FunctionLiteral 来源。捕获局部仍在名称阶段明确拒绝。

函数类型作为返回类型必须加括号，例如 `func choose() (func(i32) i32)`。
这是 `baseline/LEXICAL_SYNTAX.md` 的既定要求；不将拒绝未加括号形式认作 parser 缺陷。

公共 HIR 字段/API 未改，仅更新 Place 和 For init 的两处注释。

## 构建与运行

主目录已经接入 `lang_sema`、GMPXX 和 CLI 项目 check。对应的 CMake 目标为：

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(GMPXX REQUIRED IMPORTED_TARGET gmpxx>=6.3.0)
add_library(lang_sema STATIC src/sema/check.cpp)
target_compile_features(lang_sema PUBLIC cxx_std_20)
target_link_libraries(lang_sema PUBLIC lang_hir PRIVATE PkgConfig::GMPXX)
if(BUILD_TESTING)
  add_executable(lang_check_test tests/sema/check_test.cpp)
  target_link_libraries(lang_check_test PRIVATE lang_sema json-c::json-c)
  add_test(NAME sema.scalar COMMAND lang_check_test ${CMAKE_CURRENT_SOURCE_DIR})
endif()
```

GMP 为检查器实际依赖；json-c 只供测试读取已发布 cases.json，生产检查器不使用它。
前端不依赖 LLVM、CPython，也不启动 Python 或外部库。

首版子任务的独立构建命令（用于初稿验证，当前主目录集成命令见下文）：

```sh
cmake -S . -B /tmp/language-t04-build -DCMAKE_BUILD_TYPE=Debug -DLANG_ENABLE_LLVM=OFF
cmake --build /tmp/language-t04-build -j4
g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Iinclude \
  tests/sema/check_test.cpp src/sema/check.cpp src/hir/hir.cpp \
  /tmp/language-t04-build/liblang_modules.a /tmp/language-t04-build/liblang_frontend.a \
  -ltomlplusplus -ljson-c -lgmpxx -lgmp -o /tmp/language-t04-build/check_test-release
/tmp/language-t04-build/check_test-release /home/qin/Work/language-dev-20261103/compiler
ctest --test-dir /tmp/language-t04-build --output-on-failure
```

完整 ASan/UBSan 构建不借用未插桩的项目库：

```sh
clang++ -std=c++20 -O1 -g -DNDEBUG -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude tests/sema/check_test.cpp src/sema/check.cpp \
  src/hir/hir.cpp src/support/source.cpp src/support/diagnostic.cpp src/support/input.cpp \
  src/syntax/lexer.cpp src/syntax/parser.cpp src/modules/project.cpp src/modules/resolve.cpp \
  -ltomlplusplus -ljson-c -lgmpxx -lgmp -o /tmp/language-t04-build/check_test-sanitized
ASAN_OPTIONS=detect_leaks=0 /tmp/language-t04-build/check_test-sanitized \
  /home/qin/Work/language-dev-20261103/compiler
```

单项目只做静态检查：`check_test-release --project /absolute/path/lang.toml`。
测试使用 require，`NDEBUG` 不会删掉检查。独立案例从主目录只读加载，没有复制或修改其预期。

## 主目录实际验证

2026-11-03，Linux x86_64；GNU 16.2.1、Clang 22.1.8、GMPXX 6.3.0。
主代理修复了首版的长十进制字面量指数处理，并接入匿名函数、CMake 与 CLI。
原先仅检查显式指数的实现会把数学值为 1 的输入错误舍入为 0 或无穷；
现在合并小数位、有效数字和指数后再处理极值，仍直接舍入到目标 IEEE 精度。

```sh
cmake --preset frontend-debug
cmake --build --preset frontend-debug
ctest --preset frontend-debug
cmake -S . -B build/support-release -DCMAKE_BUILD_TYPE=Release -DLANG_ENABLE_LLVM=OFF
cmake --build build/support-release -j4
ctest --test-dir build/support-release --output-on-failure
```

Clang22 的 ASan/UBSan 构建配置与路径见 `docs/BUILD.md`；检查使用
`ASAN_OPTIONS=detect_leaks=0`，没有声称通过泄漏检查。

| 检查 | 实际结果 |
| --- | --- |
| 21 个独立案例 | 4 accept、14 source reject、3 unsupported，全部符合预期 |
| core/numeric，各 native64/python64 | 全部取得 VerifiedHirModule |
| 15 个共用后端源码，各 native64/python64 | 全部静态检查通过；其中 4 个 Panic 源码仅静态接受 |
| 全套真实 load/resolve/check/verify | 180 个项目检查通过；另含取消/失败状态检查 |
| 新增匿名函数用例 | 立即调用、返回、实参、嵌套、const、inout 通过；非法返回和命名实参拒绝 |
| 长浮点字面量 | 3 类输入各 f32/f64 的精确位模式均正确舍入为 1；保留原 tie/subnormal/zero/overflow 测试 |
| GNU Release/NDEBUG CTest | 14/14 通过，运行时 require 保留 |
| Clang22 ASan/UBSan CTest | 14/14 通过，detect_leaks=0 |
| 新 checker/HIR 实现 Werror | GNU O2/NDEBUG、Wall/Wextra/Wpedantic/Werror 编译通过 |
| HIR 测试 | 74 项损坏检查及正例通过 |
| 一元运算、丢弃参数、局部常量 | 连续负号按定宽类型运算，右侧收到期望类型；重复 `_` 形参可用但不接受 `_:value`；局部 const bool 用于可达性分析 |
| CMake/driver | 真实项目 check、backend 覆盖、JSON 诊断、源码/配置错误退出码检查通过 |
| 源语言执行、代码生成、运行时 Panic | not_run |

独立静态审查提出的五项问题均由主代理实测并修复，T04 标量检查阶段已完成。普通参数重新赋值仍等待用户选择；
当前对此操作报告未支持。T05/T06 应接收 VerifiedHirModule，使用已确定的类型、
位模式、源码求值顺序、作用域/跳转和 inout Place；不能用静态检查成功代替后端执行。
