# T10 CPU 实测记录

本记录只适用于 Linux x86_64、glibc 2.44、Python 后端 64 位、CPU。精确版本、wheel 文件名、公开来源和 SHA-256 见 [toolchain.lock.json](../contracts/toolchain.lock.json) 的 `python_cpu`。

CPython 为 3.12.15，NumPy 为 2.5.4，PyTorch 为 2.15.0+cpu。锁中包含这两个包及其 9 个运行依赖；pip 25.0.1 是额外安装工具。11 个 wheel 的完整摘要、安装后的顶层 METADATA/WHEEL 与保存的公开索引记录一致。这次没有重新下载，也没有逐个核对安装目录中的所有 `.py`、`.so` 文件。

锁中的其他字段保留 T00 当时的记录；本次只新增 `python_cpu`。编译器尚未读取这个锁来自动拒绝版本不符的环境。运行测试时必须明确传入安装了这些依赖的解释器；不要把 venv 符号链接解析为没有这些包的基础解释器。

## 绑定和宿主接口

原始 `contracts/python_bindings.example.json` 的 SHA-256 为 `44924c3c33c5acff0e604f126fa1dcb346472fa0a7966de14dd0a5b4a10adb5f`，文件仍保留 `draft_requires_conformance`，未改字节。这里按这个摘要记录实际检查结果，不将其推广为任意 Python 库支持。

宿主接口检查实际运行了 15 个声明 callable、2 个 dtype 值、8 个宿主类型、3 个存储/迭代协议和 6 个宿主错误对照。第九个类型 `lang.py.Error` 属于语言运行库，由生成程序测试另行检查；旧宿主报告中的相关 `not_run` 保留原意，没有改写为通过。

宿主报告 SHA-256：`332d00e1e98187d35ac6acb0cbb2bcc118cf3f9e5378a3f10730033c85c72696`。API 生成及运行库已在版本 `889577800cebcabada07edba2ee6cf3487289d67` 整合；生成函数检查参数顺序、省略、共享身份、typed error/cause/traceback/源位置及清理，并证明 Panic、KeyboardInterrupt、SystemExit 不被包装为 `py.Error`。

## 原版训练样例

未修改的 `04_numpy_torch.lang` 经正式 CLI `check/build --std-binding BUNDLE` 生成，生成后再与同项目同绑定的 API 输出逐字节比较。CLI 输出目录只有 `program.py` 和 `langrt.py`，没有用手写参考或 API 文件替换 CLI 输出。生成名由 `Output.symbols` 确定。

- x/y 均为 `(128, 1)`、CPU float32，满足 `y = 2x + 1`；固定 seed 7、Linear(1,1)、SGD lr 0.1。
- forward hook 和 MSE 调用各 201 次，backward、zero_grad、step 各 200 次；参数实际变化。
- 初始 loss 为 `3.829054355621338`，最终为 `9.684752999561397e-13`，有限、非负、下降且小于 `1e-3`。
- 返回真实的零维 CPU float32 Tensor，`requires_grad=True`、`grad_fn=MseLossBackward0`；未插入 item/detach/cpu/numpy。
- 与未修改手写参考在同一环境的结果使用 `rtol=1e-5, atol=1e-6` 比较；独立 CLI 复核读取已核实的参考结果，未再次运行参考。
- 直接执行 CLI 生成的 `program.py` 也退出 0，标准输出和错误输出为空。它会执行原 main 中的另外一次 200 步，独立观察器的一次 train 与直接入口运行分别记录。

原 04 SHA-256：`beb672124e6ccc697c8c129d2f7aa3bf21640de0b4af7a058eefa907fc6e6797`。原参考 SHA-256：`991e8692a123e68ad2a1e1941f39efdc8590e91b09050450a1323d739fc7080c`。运行库为 `compiler-dev/1-python-values/5`。

上述生成程序的观察器直接检查进入模型的 Tensor x/y。NumPy 中间数组的 `(128, 1)`、float32 和 `y = 2x + 1` 由同一声明集的宿主接口检查记录；没有把 Tensor 观察写成直接观察了生成函数的 NumPy 局部变量。另有生成程序小例对真实 ndarray/Tensor 的普通参数、let 保存和返回逐项检查身份及别名写入。

## 本轮整合

T10 的上述首批 CPU 范围已完成。主目录 GNU Debug/LLVM ON、GNU Release/LLVM ON、Clang 22 ASan+UBSan+LSan/LLVM ON 各 43/43，GNU Debug/LLVM OFF 为 33/33，共 162 项，无跳过。每种构建实际执行 14 个绑定/运行库用例及 7 个 CLI 用例；真实对象身份/修改 19 项、真实 NumPy 异常 16 项检查全为 true。

辅助程序曾把缺值后的 `--check-only` 当作绑定文件名，误生成 872 字节对象。修复后缺值退出 2，新对象不生成、旧对象字节保持；`--opt` 也不再被吞。新增测试的未知 `--only` 名称曾把零用例算成功，现已报错退出 2；混合选择中的合法用例可能在最终报未知名之前执行，这只影响测试脚本。

主目录整合记录 SHA-256：`4dd885865f1bbbc0748dc2ac4de000d4ee2f4731911f36c0abcb03c8bef52833`。完整记录保留在开发工作目录的 `checks/t10-cli-root/integration-final/`；源码摘要、命令、退出码、JUnit 和各用例结果分别保存。后续构建可设置 `LANG_T10_PYTHON` 后运行 `ctest --test-dir BUILD --output-on-failure`；缺少这个专用环境时两项 T10 测试不注册，不能据其他测试通过声称 T10 已在该环境验证。

本记录不覆盖 GPU、AMP、DDP、torch.compile、任意 PyTorch API、其他平台或完整原生 ARC。原生后端继续明确拒绝 Python 外部源。
