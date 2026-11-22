# 自举基础模块接口

这些模块独立于 AST/HIR，负责人可在自己的包定义公开记录。逻辑导入名为 `app/wzcc/bigint`、`app/wzcc/toml`，使用当前 Woozy 语法。不可调用旧编译器的常量求值或清单解析。

## bigint

使用 `BigInt { pub sign i32; pub limbs Vec[u32]; }`，limbs 为小端 base 2^32，零为 sign=0 且空 limbs；非零 sign 为 ±1，最高 limb 非零。公开操作不得修改实参共享的 Vec。返回整数均规范化。

- `zero() BigInt`、`from_i64(value i64) BigInt`、`from_u64(value u64) BigInt`
- `parse_digits(value string, base u32) BigInt throws Error`：base 为 2..36，允许开头 ±；只接受数字，不含前缀或下划线（调用方先处理词法格式）。
- `format_decimal(value BigInt) string`、`compare(left BigInt, right BigInt) i32`
- `negate(value BigInt) BigInt`、`absolute(value BigInt) BigInt`
- `add/subtract/multiply(left BigInt, right BigInt) BigInt`
- `divrem(left BigInt, right BigInt) DivRem throws Error`，DivRem 有 pub quotient/remainder BigInt；商向零截断，余数与被除数同号。
- `shift_left(value BigInt, bits u64) BigInt`、`shift_right(value BigInt, bits u64) BigInt`：按绝对值移位并保留符号，右移截断；它不是源语言负数算术右移，后续常量求值须按定宽规则另行处理。
- `bit_length(value BigInt) u64` 按绝对值计算；`gcd(left BigInt, right BigInt) BigInt` 返回非负结果。
- `to_i64/to_u64(value BigInt) i64/u64 throws Error`：越界报 Error.Overflow，禁止绕回。

Error 包含 InvalidBase、InvalidDigit(offset usize)、DivisionByZero、Overflow。不允许用 f64 中转大整数；先完成这些基础运算，再接入精确有理数和 IEEE 舍入。

## toml

实现当前 toml++ 所接受的 TOML 1.0 结构规则；保存字符串、整数/浮点原始文本、布尔、日期时间、数组和表。公开入口 `parse(input string) Document throws Error`，Document 有 pub root Value；Value 为带类型的 enum，表是有序 `Vec[Entry]`，Entry 有 pub key string/value Value。Error.Syntax 带 pub 风格的载荷 offset usize、message string，offset 是 Unicode 码点位置；调用方将其转换为 Source 原始字节区间。

保留重复键/重复表、点分键、隐式表与 inline table 的正确限制，不能用逐行 split 替代。支持 basic/literal/multiline 字符串、Unicode 转义、注释、数组/inline table、array of tables、数字与日期时间；不认识或尚未移植的合法形式必须明确报错，不能静默跳过或当成字符串硬猜。模块只负责通用 TOML，清单字段白名单、路径与依赖语义由 project 模块负责。

这份接口并不表示模块已实现；必须由真实 Woozy 程序和独立期望/第三方解析对照检查后才能采用。

## digest

`app/wzcc/digest` 提供 `sha256(bytes Vec[u8]) string`，返回原始字节的 SHA-256 小写十六进制文本。不得先将字节转换成 string 或规范化换行，不修改输入 Vec。用于沿用 bindings 的现有内容身份，不新增摘要算法或文件接口。

## number

`app/wzcc/number` 依赖已验证的 bigint，不依赖 AST/HIR。公开字段与函数如下（BigInt 写作 `bigint.BigInt`）。

```text
Rational { numerator BigInt; denominator BigInt; } // 分母为正，约分，零为 0/1
IeeeKind { Finite; Infinity; NaN; }
Ieee { kind IeeeKind; negative bool; significand BigInt; shift i64; }
// finite magnitude = significand * 2^shift；significand 非负，单独保留 -0
IntegerValue { value BigInt; bits u64; } // 数学值及目标宽度二补码，零扩展
Error { InvalidLiteral(message string); InvalidWidth; ZeroDenominator;
        NonFiniteInteger; IntegerRange; }
make_ratio(numerator BigInt, denominator BigInt) Rational throws Error
ratio_add/ratio_subtract/ratio_multiply(left Rational, right Rational) Rational
ratio_divide(left Rational, right Rational) Rational throws Error
ratio_compare(left Rational, right Rational) i32
parse_integer_spelling(spelling string) BigInt throws Error
ieee_float_bits(spelling string, negative bool, width u32) u64 throws Error
exact_ratio_to_ieee(negative bool, numerator BigInt, denominator BigInt, width u32) u64 throws Error
decode_ieee(bits u64, width u32) Ieee throws Error
convert_ieee(bits u64, from_width u32, to_width u32) u64 throws Error
integer_to_ieee(value BigInt, width u32) u64 throws Error
truncate_ieee(bits u64, width u32) BigInt throws Error
checked_integer(value BigInt, width u32, signed bool) IntegerValue throws Error
wrap_integer(value BigInt, width u32, signed bool) IntegerValue throws Error
```

浮点 width 仅 32/64；整数 width 为 8/16/32/64。`exact_ratio_to_ieee` 的 numerator 非负、denominator 为正；非法值报 ZeroDenominator 或 InvalidLiteral。`Ieee` 的非有限值不使用 significand/shift。保留同宽转换全部 bits；跨宽 NaN 使用现有 C++ 规范值，不承诺 payload。f64/f32 运算每一步须舍入，基础包不能经宿主浮点转换或用 f64 近似大整数。有理数基础运算以合法记录为前提；除零抛错。

实现对照 `src/hir/literal_value.cpp`、`src/hir/constant_value.cpp`、`src/sema/check.cpp`。错误码和 Source span 由后续常量求值调用方添加。动态移位和常量求值工作量限制由调用方保留，不新增语言规则。

## input / paths

`app/wzcc/input` 依赖 SourceManager，公开 `ReadResult { pub state model.PhaseState; pub failure model.FailureKind; pub diagnostics Vec[model.Diagnostic]; pub has_value bool; pub value model.Source; }` 和 `read(path string, state inout source.Manager) ReadResult`。行为对应 `src/support/input.cpp`：文件 I/O 失败返回 Environment/E_INPUT_IO/toolchain；非法源文件返回 Source/E_INVALID_SOURCE/lex，span 指向快照原始字节中的错误区间；有效源文件返回 Complete。失败时 value 不用于下游分析，SourceManager 仍保留已读到的非法快照。不要绕过已有 Source 验证。

`app/wzcc/paths` 公开 `canonical(value string) string throws os.Error`，对应 Linux 已存在路径的 `std::filesystem::canonical`；解析符号链接和相对路径，不能先 clean 掉符号链接之前的 `..`，保留 ENOENT/ENOTDIR/ELOOP 错误。`join(left string,right string) string` 只拼接路径，right 绝对路径时替换 left；不消除 `.`/`..`，避免提前改变符号链接含义。目录枚举和排序由 project 负责。

## jsondoc

`app/wzcc/jsondoc` 读取已经验证的 model.Source，公开 `parse(input model.Source) Document throws Error`。Document 有 pub root std/json.Value、pub spans Vec[LocatedSpan]；LocatedSpan 有 pub pointer string/span model.Span。pointer 使用 JSON Pointer 的 `~0`/`~1` 转义，根为 ""；span 记录原始 JSON 字节里值的完整区间，不含区间外空白，顺序为现有 index_json 的后序。

Error.Invalid(message string,span model.Span) 供 bindings 包装成 E_BINDING_INVALID。严格按现有 JSON 输入行为拒绝尾随内容、重复的解码对象键和 NUL 键；错误位置保留原编码字节（重复键指第二个完整带引号键）。普通值里的转义 NUL 仍允许。Source BOM/CRLF 不得改变原 JSON 的摘要或 span。使用 std/json 的已有 Value 与字符串/数字处理，不另定义 JSON 值系统；它默认替换重复键，必须在替换前检查。参考 project.cpp:338–400 的索引顺序和错误消息。
