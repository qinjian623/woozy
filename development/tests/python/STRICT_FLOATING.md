# Python 严格标量浮点

本项为任务 T06 补充，遵循契约 `compiler-dev/1` 与运行库版本 `compiler-dev/1-python-values/7`，开发起点为 `c58730e`。当前仅声明已验证的 Linux x86_64、Python/64 和 CPython 3.12.15 环境，导入生成模块时旧版本运行库会被版本检查拒绝。下文的构建数字和独立交付过程保留首次实现浮点时的记录，不代表当前完整测试数量。

支持 f32/f64 的 `+ - * /`、取负、六种比较和四种复合赋值。每次运算独立执行 round-to-nearest ties-to-even，保留 subnormal、符号零、Inf 和 NaN 分类，但不承诺 NaN payload/sign 和浮点异常标志。整数、tuple、Cell/Field、字符串以及 typed throws/catch/defer 沿用原实现。显式 float->int、int->float 和 f64->f32 转换由 `tests/numeric` 覆盖（运行库对应新增私有 `float_to_int`/`int_to_float`/`float_to_float` helper，版本升至 /7）；其中 f64->f32 按精确整数比做一次 RNE，有限值溢出为带符号 Infinity。当前 Vec、move 和聚合执行范围见 [T09_EXECUTION.md](../../docs/T09_EXECUTION.md)，已声明的 Python 宿主引用见 [T10_PYTHON.md](../../docs/T10_PYTHON.md)。本次浮点实现未新增语言关键字。

## 舍入依据

运行库只导入标准库 `math`、`struct` 和 `sys`。Python 中将 f32 保存为能精确表示该值的 binary64，f64 直接保存为 binary64，并通过 `as_integer_ratio()` 精确取得有限输入的整数分子与分母。加减乘除全部只用整数计算，不先做宿主浮点算术。

`_float_round` 用整数位数加一次比较算出 `floor(log2(n/d))`。设尾数位数 fraction bits 为 p（23/52）、最小正规指数为 emin（-126/-1022），取量化步长 `2**(max(exponent, emin)-p)`，按该步长缩放精确分子/分母后执行 `divmod`：

- 余数的两倍小于分母时，保留商；大于分母时，商加一。
- 两者相等时，只在商为奇数时加一。
- 商产生进位时提高指数，超过最大指数则输出 Inf；小于隐含首位的商直接构成 subnormal 位模式，舍入到零时保留单独计算的符号。

这样每次运算只发生一次目标精度舍入，除法的全部余数都会参与判断，没有 double-rounding 中间值，也不依赖宿主加减乘除的舍入模式、FTZ/DAZ 或 FMA。`struct.unpack` 只用于重建已确定的位模式，`struct.pack` 在取负时只读取已表示的值。有限数算术基于大整数计算，会比宿主浮点慢；本实现不作性能声明。

NaN、Inf 和除零在取 ratio 之前单独处理：`0/0`、`Inf/Inf`、`0*Inf` 以及异号无穷相加产生 NaN；非零数除以带符号零产生相应符号的 Inf。乘除得到零时的符号为输入符号异或；RNE 精确相消得到 +0，两个 -0 相加保留 -0。取负直接翻转符号位；六种比较使用 Python 对精确输入值的 IEEE 比较结果。整数除零仍走原 `binary` 并抛 `Panic`，不会捕获 Python 的 ZeroDivisionError/OverflowError 来冒充语言 Panic。

emitter 根据已验证 HIR 的操作数类型选用 `float_binary/float_neg`，复合赋值也走同一函数。每次运算单独生成调用，所有中间值仍按源码次序存入临时变量。编译器本身不执行 Python，也不加载 APFloat 来计算运行结果。

## 测试

`python.floating` 已加入本目录 CMake，根 CMake 未改动。

- 原 `tests/floating` 的 18 个项目、76 个探针和全部预期保持不变。12 个正例（含两个 baseline 字节副本）经正式 CLI 完成 check 和 build，由锁定的 CPython 编译完整 program.py 与 langrt.py 后再独立执行 main；六个反例校验编译器拒绝、不生成新输出且不改动旧输出。
- helper 同样执行完整的 load/resolve/check/verify/emit 流程并输出具体 HIR 函数类型，其生成的 Python 文件必须与正式 CLI 逐字节一致，观察器直接调用 CLI 产物中的函数。参数按指定 IEEE bits 解码，返回值核对分类、符号和位模式；f32 还额外检查返回值本身已处于目标精度，避免测试阶段的 pack 掩盖未舍入的结果。
- `floating_vectors.tsv` 是 `make_floating_vectors.py` 调用测试专用 LLVM 22.1.8 `float_oracle.cpp` 生成的 3,362 条独立预期，随机输入种子固定为 20261104。每条记录保存输入位模式、操作、分类、输出位模式或布尔值以及符号，覆盖两种宽度的动态除法、正规与非正规交界、溢出、符号零、非整齐除数及全部运算和比较。开启 LLVM 时逐行重算校验该文件；关闭 LLVM 时直接复用同一份固定预期，不依赖运行库自行算答案。
- 补充源码用例覆盖复合赋值、源码求值顺序、命名参数、短路求值、泛型、函数值、lambda 以及 POD 字段和副本。两种宽度分别验证 return/throw 的值保存、精确错误类型键、清理写入、浮点除零后继续执行清理、整数 Panic 跳过清理、普通返回 101 与未处理错误。
- 独立 rounder 测试检验在中点上下相差 200 位的精确 ratio，并确认版本不匹配以及受控宿主 ZeroDivisionError/OverflowError 保持原有宿主错误分类。编译阶段 PATH 中只放会失败的 Python 启动器，执行阶段则使用解释器绝对路径。

原有 Python 测试里的两个标量浮点拒绝和两个 POD 浮点拒绝保留原源码并改为正常运行。`string_cases.py` 仅跳过 Python 的旧 float 拒绝，由 `python.floating` 执行该原源码；native 侧的同条拒绝保持不变。原有的整数、T08、tuple、Cell 和字符串测试继续照常执行。

GNU Debug/Release 的验证命令和逐项实测记录见外层 `/tmp/language-strict-float-python-20261104/report.md` 与 `summary.json`。最终 ASan/UBSan、LLVMoff 和跨后端对照由 root 负责执行，本任务阶段不把它们计为通过。

## 临时共享修改

独立 worker 测试期间，`src/hir/hir.cpp` 仅对 Python 放开浮点运算，native 仍保持拒绝；类型、效果和 HIR verifier 规则未改动。两个共享测试只追加了 Python 通过检查，原有的 native 拒绝和 141 项 f32 检查均保留。外层同时交付了各自补丁、测试时 hash 和恢复后 hash。交付结束时这三个文件已恢复为 c58730e 的原字节，由 root 合并两端实际支持检查后再做集成。

## 主代理整合检查

共享检查现已同时启用两端浮点，旧总说明及 runtime/3 记录已同步更新。GNU Debug、Release 和 Clang22 ASan/UBSan 完整测试均为 34/34 通过，LLVM 关闭构建为 26/26 通过。17 份相同源码在三种构建中完成了 9660 项 native O0/O2 与 Python 返回结果对比，涵盖原 76 个探针、动态输入和清理用例。对比时 NaN 只核对分类，有限数和无穷比对位模式，零另查符号；两个 baseline 副本均已实际运行。上述结果替代本文件独立交付阶段中最终整合的 not_run 记录，原阶段报告保留。

## 浮点比较常量

f32/f64 的 `== != < <= > >=` 现在可用于 `bool` 常量初值。两个操作数先按各自确定的目标精度舍入，再按精确 IEEE 值比较；NaN 只有 `!=` 为 true，正负零相等。无穷、subnormal、负数和 f32 舍入临界点采用相同规则，不使用宿主 double 或误差容忍值计算常量。

普通变量、参数和函数调用不会因此变成常量。运行期比较仍生成原有操作，不改变转换或 Panic 规则。`tests/native/floating/const-comparison` 在 Native O0/O2 和 Python 实际执行；前端测试另核对常量使用处的 BoolLiteral 和两种精度的独立预期。
