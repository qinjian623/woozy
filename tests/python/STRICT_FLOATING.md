# Python 严格标量浮点

任务 T06 补充；契约 `compiler-dev/1`，运行库
`compiler-dev/1-python-values/7`。起点 `c58730e`。只声明已验证的
Linux x86_64、Python/64、CPython 3.12.15。旧运行库在导入生成模块时被版本检查拒绝。
下文构建数字和独立交付过程保留首次浮点实现时的记录，不代表当前完整测试数量。

支持 f32/f64 的 `+ - * /`、取负、六种比较、四种复合赋值；每次运算独立
round-to-nearest ties-to-even，保留 subnormal、符号零、Inf 和 NaN 分类。
不承诺 NaN payload/sign 和浮点异常标志。整数、tuple、Cell/Field、字符串、
typed throws/catch/defer 使用原实现。显式 float->int/int->float 及 f64->f32 转换由
`tests/numeric` 覆盖（运行库新增私有 `float_to_int`/`int_to_float`/`float_to_float`
helper 与版本 /7）；f64->f32 用精确整数比做一次 RNE，有限溢出为带符号 Infinity。
当前 Vec、move 和聚合执行范围见 [T09_EXECUTION.md](../../docs/T09_EXECUTION.md)，
已声明 Python 宿主引用见 [T10_PYTHON.md](../../docs/T10_PYTHON.md)。浮点实现没有新增语言关键字。

## 舍入依据

运行库只导入标准库 `math`、`struct`、`sys`。f32 在 Python 中保存为能够精确表示
该值的 binary64；f64 保存为 binary64。`as_integer_ratio()` 精确取得有限输入
的整数分子和分母。加减乘除只对整数计算，不先执行宿主浮点算术。

`_float_round` 用整数位数及一次比较取得 `floor(log2(n/d))`。设 fraction bits
为 p（23/52），最小正规指数为 emin（-126/-1022），选择量化步长
`2**(max(exponent, emin)-p)`。按该步长缩放精确分子/分母，然后 `divmod`：

- 余数的两倍小于分母，保留商；大于分母，商加一。
- 相等时，只在商为奇数时加一。
- 商的进位提高指数；超过最大指数产生 Inf。小于隐含首位的商直接构成
  subnormal 位模式，舍入到零时保留单独计算的符号。

因此只发生一次目标精度舍入；除法的全部余数都参与判断，没有 double-rounding
中间值，也不依赖宿主加减乘除的舍入模式、FTZ/DAZ 或 FMA。
`struct.unpack` 只重建已决定的位模式，`struct.pack` 在取负时只读取已表示的值。
有限算术使用大整数，会比宿主浮点慢；本实现没有性能声明。

NaN/Inf/零除在取得 ratio 之前分别处理：`0/0`、`Inf/Inf`、`0*Inf`、
异号无穷相加产生 NaN；非零数除带符号零产生相应 Inf。
乘除的零符号是输入符号异或；RNE 精确相消产生 +0，两个 -0 相加保留 -0。
取负翻转符号位；六种比较采用 Python 对精确输入值的 IEEE 比较结果。
整数零除仍调用原 `binary` 并抛 `Panic`。没有捕获 Python 的
ZeroDivisionError/OverflowError 来伪装语言 Panic。

emitter 从已验证 HIR 的操作数类型选择 `float_binary/float_neg`，复合赋值
也通过同一函数。每个操作单独生成调用，所有值仍按原源码次序存入临时变量。
编译器不执行 Python，不加载 APFloat 来计算运行结果。

## 测试

`python.floating` 加入本目录 CMake；根 CMake 未改。

- 原 `tests/floating` 18 个项目、76 个探针和全部预期不改。12 个正例（含两个
  baseline 字节副本）由正式 CLI check/build，锁定 CPython 编译完整 program.py
  和 langrt.py，再独立执行 main。六个反例检查拒绝、新输出不创建、旧输出不变。
- helper 也执行真实 load/resolve/check/verify/emit，输出具体 HIR 函数类型。
  它的 Python 文件必须与正式 CLI 逐字节相同；观察器实际调用 CLI 文件中的函数。
  参数按指定 IEEE bits 解码，返回值检查分类、符号和位模式。f32 还检查返回值
  已是目标精度，避免测试阶段 pack 掩盖未舍入的返回值。
- `floating_vectors.tsv` 是 `make_floating_vectors.py` 用测试专用 LLVM 22.1.8
  `float_oracle.cpp` 生成的 3,362 条独立预期。每条保存输入位模式、操作、分类、
  输出位模式/布尔值、符号；随机输入种子固定为 20261104。包含两种宽度的动态
  除法、正规/非正规交界、溢出、符号零和非整齐除数，及全部运算和比较。
  LLVM 开启时逐行重算文件；LLVM 关闭时使用同一份固定预期，不依赖运行库计算答案。
- 补充源码检查复合赋值、源码求值顺序、命名参数、短路、泛型、函数值、lambda、
  POD 字段/副本。两种宽度分别观察 return/throw 的值保存、精确错误类型键、
  清理写入、浮点除零时继续清理、整数 Panic 跳过清理、普通返回 101 与未处理错误。
- 独立 rounder 检查在中点上下相差 200 位的精确 ratio。版本不匹配、受控宿主
  ZeroDivisionError/OverflowError 保持宿主错误分类。编译阶段 PATH 只有失败的
  Python 启动器；执行阶段使用解释器绝对路径。

原有 Python 测试中的两个标量浮点拒绝、两个 POD 浮点拒绝保留原源码并改为运行。
`string_cases.py` 仅跳过 Python 的旧 float 拒绝，`python.floating` 执行该原源码；
native 的同条拒绝仍保留。原整数/T08/tuple/Cell/字符串测试继续执行。

GNU Debug/Release 验证命令和逐项实际记录见外层
`/tmp/language-strict-float-python-20261104/report.md` 与 `summary.json`。
最终 ASan/UBSan、LLVMoff 和跨后端比较由 root 执行，本任务不把它们记为通过。

## 临时共享修改

独立 worker 测试时，`src/hir/hir.cpp` 只为 Python 放开浮点运算，native 仍拒绝。
类型、效果、HIR verifier 规则未改。两个共享测试只追加 Python 通过检查，原 native
拒绝及 141 项 f32 检查保留。外层交付各自补丁、测试时 hash、恢复后 hash。
交付结束这三个文件恢复为 c58730e 原字节；root 必须合并两端实际支持检查后再集成。

## 主代理整合检查

共享检查已同时启用两端浮点，旧总说明及runtime/3记录已同步。GNU Debug、Release及
Clang22 ASan/UBSan完整测试各34/34，LLVM关闭26/26。17份相同源码在三种构建中
完成9660项native O0/O2与Python返回结果比较，包含原76探针、动态输入和清理例。
NaN仅比较分类；有限数与无穷比较位模式，零另查符号。两个baseline副本均实际运行。
上述结果替代本文件中独立交付阶段的最终整合not_run记录；原阶段报告保留。

## 浮点比较常量

f32/f64 的 `== != < <= > >=` 现在可用于 `bool` 常量初值。两个操作数先按各自确定的目标精度舍入，再按精确 IEEE 值比较；NaN 只有 `!=` 为 true，正负零相等。无穷、subnormal、负数和 f32 舍入临界点采用相同规则，不使用宿主 double 或误差容忍值计算常量。

普通变量、参数和函数调用不会因此变成常量。运行期比较仍生成原有操作，不改变转换或 Panic 规则。`tests/native/floating/const-comparison` 在 Native O0/O2 和 Python 实际执行；前端测试另核对常量使用处的 BoolLiteral 和两种精度的独立预期。
