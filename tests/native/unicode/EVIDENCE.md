# Unicode 验证

项目固定 LLVM/Clang 22.1.8、Python 3.12.15，Linux x86_64。

- 官方 NormalizationTest：20,171 行，403,420 次检查。
- 默认完整 casefold：1,606 项；官方 GraphemeBreakTest：853 项。
- Native host 和无 libc 静态程序通过；分配释放跟踪无残留，静态 ELF 无 INTERP/NEEDED/强未定义符号。
- 相同 `.wz` 公开接口在 Native O0/O2 和 Python 均运行通过，覆盖 NFC/NFD/NFKC/NFKD、casefold、NUL、emoji、RI、组合字符和拥有值保存。
- Python 数据生成连续两次字节一致，单独生成的 langrt.py 保留完整 MIT 与 Unicode 许可通知。

CTest 入口：`native.unicode_runtime`、`python.unicode18`、`values.unicode_source`。本轮整合完整检查为 122/122 通过。

直接运行：

```sh
python3 tests/native/unicode/run.py --out /tmp/woozy-unicode \
  --clang /path/to/llvm-22.1.8/bin/clang \
  --clangxx /path/to/llvm-22.1.8/bin/clang++ \
  --ar /path/to/llvm-22.1.8/bin/llvm-ar
python3.12 tests/python/unicode18_test.py
python3.12 tests/native/unicode/run_source.py --cli /path/to/woozyc \
  --root "$PWD" --python /path/to/python3.12 --out /tmp/woozy-unicode-source
```

本机详细命令与结果保存在工作目录 `checks/extensions-20261111/` 的 `unicode-llvm22/report.json`、`unicode-python-root.log`、`unicode-source-both3/report.json`、`full-ctest.log`。这些检查目录不随源码发布；上面的脚本可重新生成报告。
