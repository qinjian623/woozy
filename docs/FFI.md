# 调用 C 库

Woozy 程序可以按声明的 C 签名调用目标平台的 C 库。Linux x86_64 的绑定目标是 `x86_64-linux-gnu`，库可以是 `.a` 或 `.so`。Wz 版编译器在 macOS arm64 上用 `arm64-apple-darwin` 绑定 `.a` 或 `.dylib`。Python 后端不支持 C 绑定。

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

绑定 JSON 顶层必须正好包含 `schema_version`、`contract`、`abi`、`target`、`requires_hosted`、`modules`、`types`、`callables`，嵌套对象也不接受未知键。`schema_version` 为 `2`、`contract` 为 `compiler-dev/1`、`abi` 为 `c`。`target` 必须与输出程序的 C ABI 匹配。

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

普通 callback 参数接受直接写出的无捕获、不抛错函数名，完整签名必须与 callback 声明一致。C 可以保存函数地址并从自己的线程调用；这不会保留 Woozy 局部变量。捕获闭包、函数值变量和绑定方法使用下面的注册接口。

### 让 C 库保存捕获闭包

C API 必须同时接收函数指针和 `void *userdata`，返回不透明指针句柄，并提供注销函数。绑定中的 callback 仍声明完整 C 签名；资源和注册函数增加如下字段：

```json
{"id":"events.Subscription","module":"events/lib","name":"Subscription",
 "kind":"resource","registration":{
   "callback":"events.Callback","unregister_symbol":"events_unsubscribe"}}
```

```json
{"id":"events.subscribe","module":"events/lib","name":"subscribe",
 "symbol":"events_subscribe","parameters":[
   {"name":"callback","type":"events.Callback"},
   {"name":"userdata","type":"PtrMut[Unit]"}],
 "result":{"type":"events.Subscription","ownership":"owned"},
 "registers":{"callback_parameter":"callback",
   "userdata_parameter":"userdata","callback_userdata_parameter":"context"}}
```

假设 `events.Callback` 的 C 参数是 `value i32, context PtrMut[Unit]`，Woozy 中传入的函数只接收 `value`。编译器管理 userdata，程序不需要创建裸指针：

```wz
let values Vec[i32] = [7];
let sub = events.subscribe(func(value i32) i32 {
    return values[0] + value;
});
// sub 离开作用域时自动注销；此前 C 可以继续调用这个闭包。
```

注册句柄只能移动，不能复制。注册期间保留闭包及其捕获值；注销等待正在执行的回调和借用句柄的 C 调用结束，再释放捕获值。若绑定另外导出了注销符号对应的“单个资源参数、Unit 返回”函数，也可提前调用它；重复注销无效，注销后不能再调用使用该句柄的 C 操作。在自己的回调内注销同一注册会 Panic。

资源声明不填写 `release_symbol` 或 `retain_symbol`。两处 userdata 都必须是 `PtrMut[Unit]`，可以位于各自参数列表的任意位置。回调不能 `throws`。注册失败以空指针表示；此时 C 必须已停止并等待所有回调结束。正常注销也必须保证返回后再无回调，包括尚未进入 Woozy 的派发。绑定声明不能替代 C 库的这些保证。C 注册仍保留捕获图时，GC 不会替程序注销；这类外部注册形成的强引用循环需要保留句柄并显式 close，之后才能由 Woozy 回收。

在 Linux hosted 程序中，若 async 执行器属于另一线程，外部 C 库直接调用 libc `fork` 会产生 Panic。在 macOS 上，C 库需要调用导出的 Woozy 函数，由它通过 `std/process.fork` 创建子进程，让运行库在子进程中恢复自己的状态。`std/process.fork` 失败时抛出 `os.Error`。启动其他程序可用 `std/process.spawn`。

## 5 当前限制

- 不支持 C 可变参数、数组/union/packed/bitfield record，也不支持在 Woozy 中构造 record 或修改 record 字段。
- 只接受 C 符号。C++ 库可以提供 `extern "C"` 接口，再按 C 声明使用。当前支持 Linux x86_64 和 macOS arm64 的 C ABI。直接调用 C++ ABI、名称修饰符号或 C++ 类型不受支持。
- 保存捕获闭包仅支持上述带 userdata、返回指针句柄的注册接口；不支持抛错 callback、状态码加输出参数的注册接口，或没有 userdata 的 C API。TLS 专用 ABI 不作为公共库接口提供。
- 清单不提供任意链接器参数，也没有跳过库名或目录校验的入口；能链接什么由经过校验的 native 依赖决定。

更多语言规则见 [语言规则](SEMANTICS.md)，标准库接口见 [标准库](STDLIB.md)，系统接口见 [系统接口](OS.md)，可运行项目见 [示例目录](../examples/)。
