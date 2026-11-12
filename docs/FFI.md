# 调用 C 库

Woozy 程序可以按声明的 C 签名调用本机 C 库。当前只支持 Linux x86_64 Native 后端，目标为 `x86_64-linux-gnu`；C 库是已经安装好的 `.a` 或 `.so`。Python 后端不支持这类 C 绑定。

构建环境见 [构建说明](../development/docs/BUILD.md)，编译器实现和开发协作见 [开发资料](../development/README.md)。

## 1 一个完整的最小项目

假设 C 库已经安装成 `libwz_cadd.a`（或 `.so`），只做 `i32` 加法：

```c
#include <stdint.h>

int32_t wz_add_i32(int32_t left, int32_t right) {
    return left + right;
}
```

绑定 JSON 放在 `bindings/cadd.json`。`logical_path` 是之后导入的模块名，`name` 是 Woozy 中使用的名字，`symbol` 是真实 C 符号：

```json
{
  "schema_version": 2,
  "contract": "compiler-dev/1",
  "abi": "c",
  "target": "x86_64-linux-gnu",
  "requires_hosted": true,
  "modules": [
    {"logical_path": "cadd/lib", "provider": "native_library", "library": ""}
  ],
  "types": [],
  "callables": [
    {
      "id": "cadd.add_i32",
      "module": "cadd/lib",
      "name": "add_i32",
      "symbol": "wz_add_i32",
      "parameters": [
        {"name": "left", "type": "i32"},
        {"name": "right", "type": "i32"}
      ],
      "result": {"type": "i32", "ownership": "value"}
    }
  ]
}
```

`modules[].library` 可以为空；实际链接的库由 `dependencies.*.libraries` 指定。这里把库放在项目的 `lib/` 目录：

```toml
schema_version = 1
[package]
name = "ffi_guide"
source_root = "src"
entry = "app"

[build]
backend = "native"
word_bits = 64

[build.native]
runtime = "hosted"

[dependencies.cadd]
kind = "native"
binding = "bindings/cadd.json"
libraries = ["wz_cadd"]
library_dirs = ["lib"]
```

程序导入绑定模块并调用：

```wz
package app;
import "cadd/lib" as cadd;

func main() i32 {
    let answer = cadd.add_i32(2, 3);
    if answer != 5 {
        return 1;
    }
    return 0;
}
```

检查、构建和运行：

```sh
woozyc check --project lang.toml --backend native
woozyc build --project lang.toml --backend native --out-dir build/native
./build/native/program
```

程序退出 0，没有输出。`requires_hosted: true` 表示该绑定需要 hosted 运行环境，因此清单必须写 `[build.native] runtime = "hosted"`。省略时默认仍是 `freestanding`，加载这种绑定会在检查阶段以 `E_CAPABILITY_UNAVAILABLE` 拒绝；默认 freestanding 核心程序不变。

## 2 清单、模块和导出名

依赖键（上例的 `cadd`）是导入路径的第一段。`logical_path` 必须是完整模块路径，例如 `cadd/lib`；`import "cadd/lib" as cadd;` 后就能使用该模块导出的类型和函数。

native 依赖只接受 `kind`、`binding`、`libraries`、`library_dirs`。库名只写 `wz_cadd`，不能写 `-lwz_cadd`、文件路径、含 `/` 或 `\` 的名字，也不能以 `-` 开头；`library_dirs` 相对清单所在目录，目录必须存在。清单只接受经过校验的库名和目录，不提供任意链接器参数入口。

绑定 JSON 顶层必须正好包含 `schema_version`、`contract`、`abi`、`target`、`requires_hosted`、`modules`、`types`、`callables`，嵌套对象也不接受未知键。`schema_version` 为 `2`、`contract` 为 `compiler-dev/1`、`abi` 为 `c`、`target` 为 `x86_64-linux-gnu`。

每个 callable 的 `id` 是点分标识，`name` 和 `symbol` 都是标识符，`parameters` 是固定顺序的 `{name,type}`，`result` 是 `{type,ownership}`。声明必须与 C 库的真实原型一致；编译器不读取 C 头文件。C 调用只能传普通位置参数，每个形参一个实参，不接受命名参数、可选参数或 C 可变参数。

## 3 类型和调用

| 声明 | 在 Woozy 中的用法 |
| --- | --- |
| 固定标量 | `bool`、`i8/i16/i32/i64`、`u8/u16/u32/u64`、`isize`、`usize`、`f32`、`f64` 按值传递；`Unit` 只表示 C 的 `void` 结果，不能作为值参数 |
| `Ptr[T]` / `PtrMut[T]` | 只读 / 可写指针，指向标量、record 或 `opaque`；`Ptr[Unit]` 是 `void *`，不能索引 |
| `Buffer[T]` / `BufferMut[T]` | 16 字节的 `{pointer, u64 length}` 借用值；`T` 必须是固定标量 |
| `record` | 有自然 C 布局的 struct，可按值传参和返回，也可读取字段 |
| `resource` | 由 `release_symbol`（及可选 `retain_symbol`）管理的拥有指针 |
| `callback` | 固定参数和结果、无 `throws` 的 C 函数指针声明 |
| `opaque` | 只能通过 `Ptr[T]` / `PtrMut[T]` 使用 |

C record 的 `size`、`align` 和字段 `offset` 必须符合自然 C 布局。可以直接读取 record 值的 `point.x`，也可以把 record 按值传给另一个 C 函数；若 record 来自指针索引，索引本身仍要在 `unsafe` 中进行。当前不能构造 C record 或给字段赋值。数组、union、packed/bitfield record 都不能这样声明。

指针和 Buffer 的索引、写入只能在 `unsafe { ... }` 中进行，写入还要求 `PtrMut` / `BufferMut`。`Ptr` 索引不检查边界，越界属于未定义行为；`Buffer` 索引会检查长度，越界产生 Panic，不能靠 typed catch 恢复。可写指针可以传给只读指针参数，反向不行。

`Buffer` 不是 `String`、`Vec` 或裸地址，也不会从它们隐式转换。用 `std/ffi` 的 `ffi.buffer[u8](pointer, length)` 或 `ffi.buffer_mut[u8](pointer, length)` 从指针和长度建立借用视图，用 `ffi.len` / `ffi.len_mut` 读取长度。窄整数提升、record 按值分类等 C ABI 细节由固定 Clang 22 编译适配层时处理。

## 4 所有权、错误和回调

`Ptr`、`Buffer` 和 record 都是非拥有值，语言不会因为它们离开作用域而释放指针目标。C 返回的借用指针即使标成 `ownership: "borrowed"`，也不会自动释放；有效期间由 C 库保证，需要释放时由声明中的 C API 负责。

`resource` 是拥有值：复制一个拥有副本会调用 `retain_symbol`，每个拥有副本清理时都会调用 `release_symbol`；目标何时析构由库在最后一次引用释放时决定。callable 返回 resource 时必须使用 `ownership: "owned"`，并给出与 resource 声明一致的释放符号。若 resource 没有 `retain_symbol`，普通复制会被拒绝；借用值不调用释放。owned 值的生命周期由声明管理。

C 调用没有 `throws` 效果，也不会自动把错误变成 `py.Error`。C 返回的错误码仍是声明类型上的普通值；`errno` 通过声明的获取接口读取，例如 glibc 的 `__errno_location()` 返回 `PtrMut[i32]`，或 getter 直接返回 `i32`。`ownership` 字段只描述值、借用、拥有或 `Unit`，不是错误处理机制。

callback 只接受无捕获、不抛错的普通函数值，并且完整签名必须与 callback 声明完全一致。当前支持 C 在一次调用中同步使用该函数指针；不支持保留回调注册表、捕获闭包、抛错 callback，也不把任意语言闭包变成 C 函数指针。

## 5 当前限制

- 不支持 C 可变参数、数组/union/packed/bitfield record，也不支持在 Woozy 中构造 record 或修改 record 字段。
- 只接受 C 符号；C++ 库可先提供 `extern "C"` 接口，再按 C 声明使用；当前不直接支持 C++ ABI、名称修饰符号或 C++ 类型。Windows、macOS 和 ARM 目标也未提供。
- 保留型 callback（retained callback）、捕获闭包、抛错 callback 均不支持；TLS 专用 ABI 不作为公共库接口提供。
- 清单不提供任意链接器参数，也没有跳过库名或目录校验的入口；能链接什么由经过校验的 native 依赖决定。

更多语言规则见 [语言规则](SEMANTICS.md)，标准库接口见 [标准库](STDLIB.md)，系统接口见 [系统接口](OS.md)，可运行项目见 [示例目录](../examples/)。
