# Selfhost toolio

`app/wzcc/toolio` 为新 CLI 提供私有输出目录、外部工具执行和两个输出的发布；实现只调用当前标准库，没有旧 C++ 前端/helper 的调用；工具路径由 CLI 选择，run 不经过 shell。

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

stage 创建 out 目录及其私有子目录，名字为 `.woozyc-<prefix>-<pid>-<secure random>`，mkdir 模式0700，EEXIST时重新取随机数重试。directory 保存绝对 out 拼写，path 是直接子目录；不会在文件系统处理符号链接之前消去 `..`。prefix 与发布文件名只接受 ASCII 字母、数字、`._-`，拒绝空串、`.`、`..`，发布文件名还必须不同。

Stage 由 stage 返回，调用方只修改 preserve，不能把它重新指向其他目录。cleanup 检查私有目录形状，只递归清理 path，先 lstat，符号链接只 unlink 本身；不清 directory，也不跟随 path 本身或内部的符号链接。preserve=true 时直接返回；清理失败忽略，包括无法解码的目录项名，可能留下私有目录。

run 要求非空 argv、绝对且非空 argv[0]，拒绝所有参数和 capture_path 中的 NUL，非法输入返回 Error.System(EINVAL)。空的普通实参仍可传递。capture_path 必须尚不存在，并由调用方放在私有目录内。run 用 O_EXCL|O_NOFOLLOW|O_CLOEXEC 创建0600普通文件；若得到0/1/2，先保留低号文件打开，再用 CLOEXEC 重开同一文件，取得>=3之后才关闭这些低号临时描述符。取得文件后立即 unlink；已有文件/链接不会被截断或删除。

run 通过 process.spawn_with 的绝对路径执行工具，继承 process.environment，不使用 PATH 搜索。stdout/stderr 共享这个普通文件，不使用等待工具退出后才读取的 pipe，因此大量输出不会因 pipe 容量阻塞。wait 重试 EINTR，随后 seek到0并读原始字节；不强制解码UTF-8。信号退出用 signalled=true、exit_code=128+signal，普通退出保留状态码。文件描述符在成功和错误分支都关闭。

publish_pair 的 first/second 是 **文件名**，源为 path/name，目标为 directory/name。先 lstat 两个目标和已存在的源，拒绝目录、普通/悬空/目录符号链接、FIFO及其他非普通文件；缺少源保留给 rename 报错。已有 first 复制到私有目录内一个独占创建的随机备份，复制旧权限；之后依次 rename first、second。第二步失败，已有first时用备份恢复，否则删除新first。恢复成功抛原 System 错误；恢复失败则 preserve=true，并抛 Rollback：

- 有旧 first：backup 是私有目录内实际保留的旧文件。
- 无旧 first、删除新 first 失败：backup 指实际残留的目标 first；此时不存在旧文件备份。

Rollback.problem 是恢复/删除失败的 os.Error。调用方应把这个实际路径报告给用户，保留目录供人工处理。正常及其他错误由调用方 defer cleanup；成功发布时备份也由该清理删除。这里只恢复已报告的 rename 错误；与现有 C++ 相同，进程中断时不保证两个文件作为一项事务发布，也不承诺 fsync 持久化。

调用时先保存普通参数，避免同时读一个值及把它传为 inout：

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

`python3 selfhost/tests/toolio/run.py --woozyc <stage0> --work-dir <独立输出父目录>` 临时链接当前生产 toolio，编译正常C++工具fixture，执行 stage0 check、O0/O2 构建及真实程序。默认工具链环境包含 LLVM22.1.8 和 legacy libxml2；CLI/CXX/work/timeout可覆盖。每条命令单独保存 stdout/stderr/退出码，超时杀整个进程组，不重用其他 runner 的目录。

2026-11-13 实跑 check 通过；O0/O2 各40项通过，另各3个进程分别关闭0、0/1、0/1/2后运行成功。覆盖合并原始输出（含NUL/非法UTF-8）、退出23、SIGTERM、2MiB stdout/stderr、大量输出顺序、缺工具、非法argv、独占capture、私有目录权限、目标目录/各类symlink/FIFO拒绝、第二rename失败后的内容/权限恢复、无旧first时移除新文件、文件名拒绝、cleanup不跟随内外symlink及preserve。

结果：仓库外 `checks/toolio-root-20261113/toolio-cp3whcmu/`。恢复操作本身再次失败的 Rollback 分支只静态检查，未用竞态或系统调用替身制造；EEXIST随机目录碰撞重试也未强制制造。没有修改标准库、CLI、状态、CMake或其他包，没有运行完整CTest。
