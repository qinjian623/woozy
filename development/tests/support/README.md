# T01 独立 support 测试

契约 compiler-dev/1。源码、诊断、阶段结果与输入模块已接入 CMake/CTest，Debug 与 Release 均已由主 agent 独立构建运行；没有实现 lexer/parser。

构建和运行，所有运行时检查使用 check，Release 下同样执行：

```sh
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude src/support/source.cpp src/support/diagnostic.cpp tests/support/support_test.cpp -o reports/support-test
reports/support-test > reports/support-invalid-source.json
c++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Iinclude src/support/source.cpp src/support/diagnostic.cpp tests/support/support_test.cpp -o reports/support-test-release
reports/support-test-release > reports/support-invalid-source-release.json
```

## 本次 API 修改

SourceManager::update 对已存在的 FileId 总是发布新快照（内存分配等宿主异常除外），包括非法源码；原始字节、文件身份、revision 和 snapshot 身份均保留。未知 FileId 仍抛 out_of_range。

SourceSnapshot 新增 valid()、error() 和 raw_span(begin,end)。error() 保存首个 SourceError，其 byte 和 length 表示原始错误区间；调用方用 raw_span(byte,byte+length) 生成词法诊断。非法多字节前缀的范围不包含后面的合法字符。raw_span 只检查原始字节范围及端点顺序，允许非法字节、码点内部和 CRLF 内部位置。

normalized()、span()、position()、byte() 只接受有效源码快照；非法快照调用这些方法会抛保存的 SourceError，不返回部分规范化文本或部分位置表。span 接受规范化字节位置，返回原始 SourceSpan；position/byte 在原始字节和零基 UTF-16 之间转换。制表符计一个单元，非 BMP 计两个。有效文本的这些转换仍拒绝码点内部、CRLF 中间及被移除 BOM 内部位置。

新增 validate_utf8(string_view)，与源码验证共用同一解码函数，只检查 Unicode 标量编码，允许 NUL、CR 和 U+FEFF 等合法字符。诊断序列化的每个 JSON 字符串都调用它，非法元数据抛 SourceError（byte 相对于该字符串），不会替换字节或返回无效 JSON。控制字符继续转义。诊断的 span 改用 raw_span 检查，因此错误源码可以带非 null primary。

其他 API 不变：强类型 ID、SourceSpan 仅在 source.hpp 定义；get 精确读取旧快照；fixit revision 必须与引用快照一致。RevisionId 序列化为十进制字符串。ExitCode 保留 TOOLING 全部退出码，由 driver 决定；Status 与环境错误退出码不是一一对应。

## 验证与限制

测试覆盖 BOM/CRLF/LF、中文/非 BMP/制表符、非法编码/NUL/独立 CR、原始错误诊断、不可变旧快照、非法位置/ID、旧 fixit、JSON 转义和各类非法 UTF-8 元数据。标准输出只有孤立续字节源码的真实诊断 JSON，错误范围为 test.wz 的 [1,2)。

未实现：编辑应用、修复原文比较、终端显示列、UTF-8/UTF-32 LSP 协商、lexer/parser、通用 IR、诊断排序和 driver 集成。每个非法快照仅记录首个输入错误。真实序列化结果已由主 agent 使用包内 schema 检查器逐项检查。


阶段与输入补充：`phase.hpp` 定义 PhaseState、FailureKind、PhaseResult<T>。
只有 complete、存在 value 且无 error 诊断时 success() 为真；调用方负责传入有效的 T。
exit_code()/status() 使用已有枚举。`input.hpp` 提供 read_source(path, manager, stop_token)。
SourceManager::update 和 SourceSnapshot 构造函数接受可选 stop_token；取消抛 Cancelled，
不发布快照、不增加 revision 或 snapshot ID。SourceManager 不支持并发访问。
取消不生成源码诊断；非法输入发布可用于诊断的原始快照，但结果不含成功 value。

独立测试（输出包含取消、missing-file、非法 UTF-8/NUL/CR 的真实 JSON）：

```sh
c++ -std=c++20 -pthread -Wall -Wextra -Wpedantic -Werror -Iinclude src/support/source.cpp src/support/diagnostic.cpp src/support/input.cpp tests/support/phase_test.cpp -o /tmp/phase-test
/tmp/phase-test
c++ -std=c++20 -pthread -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Iinclude src/support/source.cpp src/support/diagnostic.cpp src/support/input.cpp tests/support/phase_test.cpp -o /tmp/phase-test-release
/tmp/phase-test-release
```

规范化取消测试在大输入处理期间从另一线程请求停止；线程仅用于测试。
未覆盖：读取中的确定时点取消、设备 I/O 故障、分配失败、driver/CMake 集成。
E_INPUT_IO 已登记。未完整填写的阶段结果默认归为内部错误；源码/未支持/环境错误由各阶段明确分类。主 agent 的测试记录在仓库外层 checks/。
