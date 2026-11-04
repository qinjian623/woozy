# T09 Python 拥有值执行

共同版本仍为 `compiler-dev/1`；Python runtime 从 `compiler-dev/1-python-values/3` 更新为 `/4`。公共头、嵌入 runtime、生成程序版本检查和新增 runtime 测试一致更新。源码基点是 `ff3ace254080036373cd08b1f785163a8804a80c`。

`src/backend/python/emitter.cpp` 只接收公开的 `VerifiedHirModule`，保留公共支持检查。它读取 `classify_value`、`ValueUse`、`Temporary`、`LocalLifetime`、`FullExpression`、`CallLifetime` 和 `ExitPlan.actions`，没有另建公共 HIR 或从源码猜测拥有操作。

`runtime/python/langrt.py` 用 `Owner` 保存活动值和类型专用 copy/drop 描述，`Frame` 登记临时值、局部、隐藏集合和调用借用。Python 引用和 GC 不改变语言引用计数，没有 `__del__`。数值仍按既有规则处理；struct/enum 仍表示为 tuple，由具体类型描述递归复制或逆序销毁活动字段。Vec 拷贝 retain 同一容器；最后 release 逆索引 drop 元素并清空存储。

支持有限嵌套 Vec、含 Vec 的 struct/enum、具体泛型实例、整个绑定及计算临时值的 move、普通参数借用、保存或返回参数时的逻辑 copy、索引读取/赋值、切片和 for-in。构造输入按源码顺序求值，成功后按成员位置安装；失败按 HIR 中的已完成临时值逆序清理。赋值先保护接收者并计算 RHS，成功后替换旧值；失败不安装 RHS，也不恢复移动源。

调用只有在全部实参成功后才转交 move 实参。成功执行 CallLifetime；typed-error 执行该调用的临时值清理，再由词法块依次执行 defer 和局部销毁。return/throw 先保存已转交的活动输出，再清理剩余临时值。错误载体有一次活动载荷，匹配 catch 后 take；未捕获的错误由 runner 释放。Panic 仍是 BaseException，不转换成 typed-error，也不执行生成的语言清理。

for-in 只求值集合一次，持有隐藏拥有值，登记入口长度，每轮读取当前元素并取得逻辑副本。每轮局部随 continue/break/return/throw 清理；离开循环时先结束登记，再释放隐藏集合。`Index` 只保存接收者和原整数值，不保存元素地址。索引和切片检查数学值后才访问列表，不采用 Python 负索引。私有 `_replace_length` 在活动迭代期间、修改前 Panic；没有新增源语言长度 API。

保持拒绝普通参数重赋值、计算产生的拥有 switch 临时值、动态 string/Vec[string]、部分 move、非平凡 inout、索引 inout、索引字段写回、递归拥有图、捕获闭包、foreign 和原 T07 限制。原 Python 生成深度限制仍可能在 build 时报告 `E_CAPABILITY_UNAVAILABLE`。没有新增公开 len/push/share。

本次 GNU Debug/Release 分别通过 42 项源码检查：原 18 项及新增 24 项，包括 7 项静态拒绝、34 项真实 CLI 构建并执行、1 项无限 defer 仅构建。34 项执行中有 28 项普通结束、1 项未捕获 typed-error、5 项 Panic。每套另有 12 项 runtime 检查和 8 项损坏 HIR（分别经 verify_types 与 verify 拒绝），并验证拥有 HIR freeze 后不受 builder 修改影响。

私有观察程序保留宿主引用以固定对象编号，记录分配、最后释放、标量 drop、源码事件和存活计数。普通和 typed-error 结束要求无存活 Vec、无重复释放、无活动拥有槽和未结束借用；Panic 不断言清理完成。逆索引、逆字段、部分构造前缀、defer/局部顺序和进入 catch 前的清理有独立预期。分配失败用 unittest.mock 局部替换对象或元素分配函数，大小及计数溢出用私有入口，不耗尽主机内存，也没有生产配置开关。

真实执行时只临时应用 root 的 `python.patch`。两套完整 CTest 各 35 passed / 3 failed：失败项 `python.projects`、`python.aggregates`、`sema.t09` 都含“Python Vec 尚未支持”的旧断言，没有删改。`sema.t09` 在首个 Python 发布断言处停止，后续测试在这两次运行中未执行。

临时补丁已经还原，公共文件 SHA256 恢复为 `bd8e0fab69833d3a556ce502706cc689551c49701447ea7b78c3fd46c099af7f`。还原后两套完整 CTest 首轮各 35 passed / 2 failed / 1 skipped；两项失败来自临时阶段留在“预期无输出”目录的生成文件。保存并移走这些文件后，两项各自重跑通过，合计每套 37 passed / 1 skipped。T09 源码测试在公共规则拒绝时明确 skipped，未记作运行通过。

复查旧拒绝测试时要使用干净的生成目录，或先保存并移走临时支持阶段生成的文件；正确拒绝不会删除先前输出。所有失败与修正均保留在交付日志中。

交付目录为 `/tmp/language-t09-python-20261104/`，入口为 `report.md`、`summary.json`、`product.patch`、`commands.jsonl` 和 `temporary-hashes.json`。原 18 份源码及 cases.json、19 份清理输入、27 份泛型输入、76 个浮点探针与原样例未改。T09 整体仍为 in_progress；正式公共支持规则、Native T09、共同 Clang sanitizer、LLVMoff 和两后端同源比较由 root 接续。
