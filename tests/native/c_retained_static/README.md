# 静态 C 回调测试

真实 C 库与 Woozy 程序检查保留回调、外部 pthread、嵌套调用和 GC 线程状态。
CTest 名称为 `native.c_retained_static`、`native.c_retained_static_gc_program`。
GC 源码测试必须使用 `--callback-gc require`。

实现和验证范围见 [开发说明](../../../development/docs/C_RETAINED_STATIC_CALLBACKS.md)。
