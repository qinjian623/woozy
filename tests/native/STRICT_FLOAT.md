# Native 严格 f32/f64

operation_id: `language-strict-float-native-20261104`。契约 `compiler-dev/1`。
起点 `c58730e4b0bb95c85df8606b3d236342e968461e`，T08 的 CFG、错误通道和清理检查保留。

支持同宽 f32/f64 的 `+ - * /`、一元负号和 `== != < <= > >=`，也支持这些运算的
复合赋值。操作直接生成相应类型的 LLVM `fadd/fsub/fmul/fdiv/fneg/fcmp`，不先转
f64。`!=` 使用 `une`，其余比较使用 ordered 谓词。`fneg` 保留负零；浮点除零
产生 IEEE Inf/NaN，不进入整数除零 Panic。字面量继续采用 HIR 已确定的目标位模式。
没有添加公共类型、运行库入口或新的 IR 操作。

Native IR verifier 分别检查浮点合法操作、操作数 TypeId 相同、算术结果同宽、
比较结果为 bool。浮点 bitwise、shift、remainder、logical-not 以及混宽/整数混用
仍为无效 IR。现有 slot 初始化、支配关系、成功/失败侧、清理次序和不可达调用检查
没有改动。

浮点指令无 fast-math 或 contract 标志，TargetMachine 的 AllowFPOpFusion 为 Strict；
未生成 fma/fmuladd。每次操作按自己的 float/double 类型舍入，LLVM O2 不得通过
重结合或隐式乘加融合改变结果。只支持已有的 native/64 Linux x86_64、generic CPU、
空 features 与 O0/O2，工具链固定 LLVM/Clang/LLD 22.1.8。

执行环境要求 SSE MXCSR 使用 round-to-nearest ties-to-even，FTZ/DAZ 关闭，浮点异常
全部屏蔽。编译器按该环境生成普通 IEEE LLVM 指令，不在函数入口修改调用者的浮点
环境。Linux 进程默认环境满足此要求；改变该环境的 foreign 调用仍未支持。
不保证 NaN payload/sign 或异常标志；NaN 只比较分类。显式 float->int 与 int->float
转换见 `tests/numeric`（float->int 先向零截断再范围检查，int->float 一次 RNE）；
浮点宽度转换、ARC、Vec、move 和 foreign 仍明确拒绝。

## 执行检查

`native.floating` 经正式 CLI load/resolve/check/build，独立运行 ELF 的 main。
`float_probe.cpp` 再通过 VerifiedHIR/VerifiedModule 发射，要求目标文件与正式 CLI
的 program.o 逐字节相同。观察器解析该次发射的优化前 LLVM，以实际生成函数的
FunctionType 构造调用，将真实返回值 bitcast 后写出；没有通过 C++ 私有聚合 ABI
读取值。观察程序由同版本 Clang 在 O0/O2 编译执行；保留 before/after/observe LLVM、
完整 argv/退出/stdout/stderr、所有源及生成文件 hash 和每条实际位模式。观察程序先
用 stmxcsr 读取自身运行时的环境，检查上述 RNE/subnormal 要求。

- 原 `tests/floating` 的 12 个正例 main 全部运行，包括两个原 baseline 副本。
  原 76 个 probe 每个优化等级逐项调用，按原 class/bits/sign/bool 比较；NaN 不比较
  payload/sign。原 6 个反例继续拒绝，失败构建不得新建输出或改写旧输出。
- `floating/dynamic` 的 3,138 条输入覆盖两种宽度的全部操作、NaN/Inf/±0、subnormal、
  溢出、非整齐动态除法、复合赋值、每步舍入和禁止 FMA。输入及期望写入逐项结果。
  新期望由测试专用 `float_expected.py` 用 Fraction 精确有理数运算及整数 RNE 计算，
  独立于 Native 实现，不进入编译器。随机序列种子固定为 20261104。
- `floating/values-cleanup` 的 6 条观察检查函数值、泛型、POD 字段复制/修改、inout、
  命名实参与算术操作数的副作用顺序、bool 短路、浮点返回/错误及 defer 读取次序。
  包含正常值与错误值同为 f64 的两条实际分支，返回值在 defer 修改局部前保存。
- 另独立运行整数除零 Panic、普通返回 101 和未捕获浮点 typed error；区分退出码
  相同但 stderr 不同的 Panic 与普通返回。5 个新增拒绝项目保持转换/Vec/move 限制。
- 编译阶段 PATH 中 python/python3/python3.12 等启动器均失败并写 marker；确认没有
  调用过它们。Python 3.12.15 只运行测试 runner，未解释源语言或参与 Native 实现。

新增浮点源的错误 IR 变体由语言 verifier 拒绝，覆盖 bitwise、shift、remainder、
非法 unary、混宽/整数操作数、错误比较结果类型、缺操作数/结果。
原 scalar、POD、字符串及 T08 测试继续运行。

共享检查现已启用两端严格浮点，过时拒绝断言已改为实际接受检查。主代理最终
GNU Debug、GNU Release和Clang22 ASan/UBSan各34/34，LLVM关闭26/26。
正式同源程序在三种构建中比较9660项返回值（含NaN分类和布尔值），结果一致；
Python运行库为/3。原19清理、27泛型、floating及baseline输入未改。

ASan/UBSan仅检查编译器，不包含生成ELF、预编译LLVM或LeakSanitizer。
编译和测试使用固定Python3.12.15，不修改系统解释器。

## 浮点比较常量

f32/f64 的 `== != < <= > >=` 现在可用于 `bool` 常量初值。两个操作数先按各自确定的目标精度舍入，再按精确 IEEE 值比较；NaN 只有 `!=` 为 true，正负零相等。无穷、subnormal、负数和 f32 舍入临界点采用相同规则，不使用宿主 double 或误差容忍值计算常量。

普通变量、参数和函数调用不会因此变成常量。运行期比较仍生成原有操作，不改变转换或 Panic 规则。`tests/native/floating/const-comparison` 在 Native O0/O2 和 Python 实际执行；前端测试另核对常量使用处的 BoolLiteral 和两种精度的独立预期。
