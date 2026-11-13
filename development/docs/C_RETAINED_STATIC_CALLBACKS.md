# 静态 C 回调与 GC

C 可以保留无捕获、不抛错的 Woozy 函数指针，在注册调用返回后再次调用，
也可以从自己的 pthread 调用。源码实参必须是直接函数名；局部函数值变量、
捕获闭包和 throws 回调仍被拒绝。C 自己负责上下文数据和注销时等待在途调用。

## 实现

`src/backend/native/emit.cpp` 的 `callback_bridge` 在进入语言函数前分配并
清零一个32字节、8字节对齐的 `LangGcProgramCallbackToken`，调用
`lang_gc_program_callback_enter_v1`；目标返回后调用 leave。每次调用有自己的
token，嵌套调用按栈顺序恢复。

`runtime/native/gc_program.hpp/.cpp` 复用既有 world callback 接口：GC 未启用
或调用者是已完成的 main 线程时不进入；其他情况临时注册外部线程，或保存已注册线程的 SAFE
深度，结束后恢复原状态并移除本次创建的注册。没有新增回调注册表。

## 检查

`tests/native/c_retained_static/` 包含静态保留、同步图回调、外部pthread图回调
及捕获/throws拒绝项目。图回调实际创建并读取 `Vec[Vec[i32]]`，避免无图程序
跳过GC初始化后掩盖缺陷。

- `native.c_retained_static_gc_program`：直接检查token布局、GC关闭/启用、
  SAFE恢复、已注册/未注册线程和main完成后的行为。
- `native.c_retained_static`：Native O0/O2；重复和嵌套C往返、外部线程、回调
  返回后的图操作、C资源及userdata归零。CTest必须传 `--callback-gc require`，
  不允许把回调GC Panic记为unsupported后通过。

独立审查还执行了析构时回调：C资源释放函数调用Woozy，Woozy创建图并再次
调用C，O0/O2均通过。相关源码hash、命令与结果保存在Root的检查目录。

这批检查不表示捕获回调或任意C库的注销接口已经支持。
