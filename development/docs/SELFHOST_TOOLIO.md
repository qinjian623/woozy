# Selfhost toolio

`app/wzcc/toolio` 为新 CLI 提供私有输出目录、外部工具执行和双文件发布功能。实现只调用当前标准库，不调用旧 C++ 前端或 helper；工具路径由 CLI 指定，run 不经过 shell。

```wz
pub enum Error {
    System(problem os.Error);
    Rollback(backup string, problem os.Error);
}
pub struct Stage { pub directory string; pub path string; pub preserve bool; }
pub struct ProcessResult { pub exit_code i32; pub signalled bool; pub output Vec[u8]; }
pub func stage(out string, prefix string) Stage throws Error
pub func cleanup(value Stage)
pub func run(argv Vec[string], capture_path string) ProcessResult throws Error
pub func publish_pair(value inout Stage, first string, second string) throws Error
```

stage 创建 out 目录及其私有子目录，命名为 `.woozyc-<prefix>-<pid>-<secure random>`，mkdir 权限为 0700，遇到 EEXIST 时重新取随机数重试。directory 保存 out 的绝对路径写法，path 是其直接子目录；在文件系统解析符号链接之前，不会提前消去 `..`。prefix 与发布文件名只接受 ASCII 字母、数字和 `._-`，拒绝空串、`.` 和 `..`，且两个发布文件名必须不同。

Stage 由 stage 返回，调用方只能修改 preserve，不能把它重新指向其他目录。cleanup 会校验私有目录路径结构，只递归清理 path：先 lstat，遇到符号链接只 unlink 本身；不清理 directory，也不跟随 path 本身或内部的符号链接。preserve=true 时直接返回；清理失败（包括遇到无法解码的目录项名）会被忽略，此时可能遗留私有目录。

run 要求 argv 非空、argv[0] 为非空绝对路径，且所有参数和 capture_path 都不能包含 NUL；非法输入返回 Error.System(EINVAL)，但允许传递空的普通实参。capture_path 必须尚不存在，并由调用方放在私有目录内。run 以 O_EXCL|O_NOFOLLOW|O_CLOEXEC 创建权限 0600 的普通文件；如果拿到的描述符是 0/1/2，会先保持这些低号文件打开，再用 CLOEXEC 重新打开同一文件，直到取得 >=3 才关闭之前的低号临时描述符。取得文件后立即 unlink；已有文件或链接不会被截断或删除。

run 通过 process.spawn_with 按绝对路径启动工具，继承 process.environment，不搜索 PATH。stdout/stderr 共享这个普通文件，不使用等待工具退出后才读取的 pipe，因此大量输出不会因 pipe 容量阻塞。wait 遇到 EINTR 时重试，随后 seek 到 0 并读取原始字节，不强制按 UTF-8 解码。信号退出记为 signalled=true、exit_code=128+signal，普通退出保留状态码。文件描述符在成功和错误分支都会关闭。

publish_pair 的 first/second 是 **文件名**，源路径为 path/name，目标路径为 directory/name。发布前先 lstat 两个目标和已存在的源，拒绝目录、普通/悬空/目录符号链接、FIFO 及其他非普通文件；缺少源文件时留给 rename 报错。如果目标 first 已存在，会先将其内容和旧权限复制到私有目录内独占创建的随机备份文件，随后依次 rename first 和 second。若第二步失败：原本存在 first 时用备份恢复，否则删除新写入的 first。恢复成功则抛出原 System 错误；恢复失败则将 preserve 设为 true，并抛出 Rollback：

- 有旧 first：backup 是私有目录内实际保留的旧文件。
- 无旧 first、删除新 first 失败：backup 指向实际残留的目标 first；此时不存在旧文件备份。

Rollback.problem 是恢复或删除失败的 os.Error。调用方应把这个实际路径报告给用户，并保留目录供人工处理。正常情况和其他错误由调用方通过 defer cleanup 清理；成功发布时备份也会随之删除。这里只恢复已返回的 rename 错误；与现有 C++ 实现一致，进程中途退出时不保证两个文件作为事务一起发布，也不承诺 fsync 持久化。

调用时先保存普通参数，避免同时读取某个值又将它作为 inout 传入：

```wz
var output = toolio.stage(out_directory, "native");
defer { toolio.cleanup(output); }
let capture = output.path + "/tool-output";
let arguments = [absolute_clang, "-c", input_ir, "-o", output.path + "/program.o"];
let process_result = toolio.run(arguments, capture);
// 检查 process_result 后，在 output.path 内准备好两个输出。
toolio.publish_pair(inout output, "program.o", "program");
```

## 实际检查

`python3 selfhost/tests/toolio/run.py --woozyc <stage0> --work-dir <独立输出父目录>` 临时链接当前生产 toolio，编译正常的 C++ 工具 fixture，执行 stage0 check、O0/O2 构建及实际程序。默认工具链环境包含 LLVM 22.1.8 和 legacy libxml2；CLI/CXX/work/timeout 均可覆盖。每条命令单独保存 stdout/stderr 和退出码，超时会杀掉整个进程组，不复用其他 runner 的目录。

2026-11-13 实跑 check 通过；O0/O2 各 40 项通过，另各有 3 个进程分别关闭 0、0/1、0/1/2 后运行成功。覆盖合并原始输出（含 NUL/非法 UTF-8）、退出码 23、SIGTERM、2MiB stdout/stderr、大量输出顺序、缺工具、非法 argv、独占 capture、私有目录权限、目标目录/各类 symlink/FIFO 拒绝、第二次 rename 失败后的内容与权限恢复、无旧 first 时移除新文件、文件名拒绝、cleanup 不跟随内外 symlink 以及 preserve。

结果保存在仓库外 `checks/toolio-root-20261113/toolio-cp3whcmu/`。恢复操作本身再次失败的 Rollback 分支只做了静态检查，未用竞态或系统调用替身触发；EEXIST 随机目录碰撞重试也未强制触发。本次未修改标准库、CLI、状态、CMake 或其他包，未运行完整 CTest。
