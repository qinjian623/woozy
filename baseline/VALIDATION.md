# 验证结果

验证日期：2026-11-03。

## 已完成

| 检查 | 结果 |
| --- | --- |
| 词法与语法边界自测 | 59 / 59 通过 |
| 完整正例 | 7 / 7 通过语法检查 |
| 词法或语法负例 | 5 / 5 按预期拒绝 |
| 语义负例 | 4 / 4 按预期通过语法阶段 |
| 全部样例分类 | 16 / 16 与清单一致 |
| Python 参考文件 | 通过 Python 的 compile 语法检查 |
| NumPy 数据准备 | 实际执行，x 与 y 均为 (128, 1)，dtype 为 float32 |
| 数据关系 | 已检查 y = 2x + 1 |

语义负例通过 parser 是预期行为。它们分别涉及未处理 throws、类型不匹配、Result 被误当业务值、清理块传播错误；这些规则不由当前语法识别器实现。

自测覆盖数字和字符串边界、分号、导入、函数类型的 throws 归属、命名参数、泛型/索引中性后缀、函数字面量及控制结构。

## 未实现或未执行

- 没有本语言的完整编译器或解释器，不能据此声称样例已编译执行。
- 未实现名称解析、静态类型检查、泛型约束求解、错误效果检查、模式穷尽性、所有权和 ARC。
- 未实现 LLVM 或 Python 代码生成。
- 当前环境未安装 PyTorch，未执行训练，未验证最终损失、梯度、GPU 或性能。
- Python 文件是手写对照程序。
- 设计文档提供 Markdown 源文件；在线版本核对内容结构，不作逐屏视觉验收声明。

## 可重现命令

```sh
python3 tools/check_syntax.py --self-test
python3 tools/check_syntax.py samples/*.lang
python3 tools/check_syntax.py --json samples/negative/04_unhandled_throws.lang
```

第三条命令应接受语法；它不代表未声明的错误传播是合法的。

详细逐文件记录见 `validation.json`。该文件记录本次检查结果，`examples_notes.json` 记录设计预期。

