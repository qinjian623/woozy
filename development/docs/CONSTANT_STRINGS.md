# 字面量与常量字符串

Native 和 Python 支持字符串字面量、顶层和局部 `const`、常量别名、`let/var` 复制、`var` 重新绑定，以及普通参数和返回。泛型、非捕获函数值、递归及跨模块、跨包的 `pub const` 使用相同规则。普通参数名只读，重新赋值报 `E_TYPE_MISMATCH`。

```lang
const Greeting string = "你好\0😀";
const Alias = Greeting;
func text() string {
    const local = Alias;
    return local;
}
```

局部声明的初始化使用声明前的环境：`{ const x=x; }` 读取外层的 `x`。普通变量和函数调用结果仍不能作为本阶段的 `const` 初值。空串、原始多行字符串、转义、非 BMP Unicode 字符和内嵌 NUL 均保留完整 UTF-8 字节与长度；NUL 不表示字符串结束。

字符串现已支持作为 struct 字段、enum 载荷、`Vec[string]` 元素，以及嵌套结构和泛型替换后的类型使用，支持保存、逻辑复制、普通传参和返回，也支持对整个局部绑定或临时字符串执行显式 `move`。`move` 后的绑定在重新初始化前不能读取；string 及含 string 的聚合仍不是可 `inout` 的 POD，字符串的 `inout`、部分字段/索引 move 以及动态拼接、索引、切片仍未实现。通过 Python 绑定，宿主可以返回合法的不可变 Python `str`，但这并不代表 Native 侧新增了动态字符串接口。

## 表示与检查

Native 使用 `{ptr, target_uintptr}`，长度来自 LLVM 目标 DataLayout。数据保存在 private constant 数组中，不添加结尾 NUL；复制、传参和返回时只拷贝指针和长度，数据在整个程序运行期间有效。字符串本身不分配动态存储，也不执行 retain/release。函数使用私有 LLVM 调用类型，不承诺 C/C++ 聚合 ABI。当前 Linux x86_64 目标的字符串大小为 16、对齐为 8、字段偏移为 0 和 8。

Python 将完整 UTF-8 bytes 解码成不可变 `str`。复制和重新绑定保留值规则，不修改原字符串。运行库接口未改变。

`StringLiteral` 保留 lexer 已解码的字节。HIR 验证器核对类型和 UTF-8，并沿真实字面量或 resolver 确定的常量初始化表达式追踪别名、逐字节校验，不接受把普通运行期读取伪造成字面量。NativeIR 的 `Op::string_literal` 同样检查类型、结果、操作数和字节；其他操作不能附带字符串内容。

string 属于无需析构的可复制值（`needs_drop=false`），布局仍为上述两个字段；含 string 的 struct/enum 和 `Vec[string]` 按真实字段递归复制。Vec 本身继续使用 ARC，字符串叶值不执行 retain/release。string 与含 string 聚合的 `pod_inout` 为 false。

## 测试

`native.strings`、`python.strings` 保留原 20 个观察函数。`native.string-constants`、`python.string-constants` 使用独立项目，新增 11 个常量观察函数，覆盖显式与推断类型、顶层/局部常量、别名、遮蔽、跨模块/包、空串、原始字符串、Unicode 与 NUL。原有字符串样例的内容未改，发布改名时只将活动文件后缀迁移为 `.wz`。

Native 测试先用正式 CLI 构建并运行，再按生成 LLVM 的真实函数类型观察返回值。辅助程序与 CLI 的目标文件必须逐字节相同；观察程序输出目标长度和完整字节，分别以 Clang O0/O2 执行。Python 在独立 3.12.15 进程中核对实际返回类型、文本、UTF-8 字节与长度。两端采用同一组冻结预期。

测试还检查常量环、运行期初值、类型错误、错误构建不产生新输出或改写旧输出，以及 HIR 字节篡改、非法 UTF-8、类型和来源损坏。每次命令、退出码、文件摘要和实际值保存在构建目录的 `string-results.json`。
