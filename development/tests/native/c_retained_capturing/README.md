# 捕获式 C 回调测试

源码在 `tests/native/c_retained_capturing/`，运行库单元测试说明见 [c_callback_runtime](../c_callback_runtime/README.md)。绑定要求见 [实现说明](../../../docs/C_RETAINED_CAPTURING_CALLBACKS.md)，语言用法见 [调用 C 库](../../../../docs/FFI.md)。

## 执行

配置构建时会注册两个 CTest：

- `native.c_retained_capturing_fixture`：真实 C 共享库及 pthread 生命周期测试。
- `native.c_retained_capturing`：Woozy 源码 O0/O2、拒绝用例、失败时无输出，以及核心 ELF 独立性检查。

在编译器仓库根执行：

```sh
ctest --test-dir build/woozy-stdlib-native --output-on-failure -R '^native.c_retained_capturing'
```

完整构建环境见 [构建说明](../../../docs/BUILD.md)。CTest 固定传 `--source-tests require`：源码未执行、构建失败或程序退出非零都会失败。独立 runner 的 `auto` 仅用于检查旧编译器是否已接入，不用作正式通过结果。

runner 为每次运行保存 `report.json`、命令、退出码、诊断和文件摘要。fixture、构建目录与库搜索目录位于独立输出目录，测试不会往源码目录生成共享库。

## 源码用例

13 个项目均在 O0/O2 下编译并执行。

| 项目 | 检查内容 |
| --- | --- |
| probe | 注册与普通调用 |
| captures_escape | Vec 捕获逃逸、函数变量、绑定方法 |
| userdata_positions | 回调 userdata 位于首、中、尾；多注册各自保存环境 |
| returns_and_units | bool、i64、Unit 返回 |
| sync_register | C 在注册函数返回前同步回调 |
| c_thread | C pthread 调用捕获闭包 |
| failure_cleanup | 空句柄失败、同步回调后失败，均释放环境 |
| close_modes | 显式与作用域结束时自动注销 |
| typed_errors | 正常返回和 throws 退出均清理 |
| gc_cleanup | 显式注销外部根环后继续使用局部值，最终产生 GC 压力 |
| two_resource_borrow | 同一个与不同的两个句柄参数，借用按逆序结束 |
| cleanup_observation | C token 计数归零，Vec/Fn 的 Weak upgrade 另确认 None |
| move_capture | move-only 注册句柄由 move func 保存；函数副本共享环境并只注销一次 |

Weak 观察独立于 C token 计数。`gc_cleanup` 不要求立即回收循环；注册持有的外部强引用必须先注销，随后按正常 GC 规则处理。

拒绝用例检查 throws 或错误签名的回调、非法 userdata 类型、非 hosted 注册、Python 后端和旧静态接口传入捕获闭包。失败构建不得留下 `program` 或 `program.py`。

## C fixture 的等待保证

C 库只保存函数指针和 userdata，不解释或释放 Woozy 环境。注销标记关闭并等待全部预留的 `in_flight`，包括已拿到 userdata、尚未进入 trampoline 的派发；所有派发结束后才释放 token。失败注册也等待其全部回调结束。

独立 C++ fixture 测试用 pthread 条件变量控制交错：先让外部线程预留一次派发，确认注销已开始等待，再放行回调，检查注销在回调结束后返回。双句柄调用不在回调期间持有全局锁，同一 token 可作为两个参数。
