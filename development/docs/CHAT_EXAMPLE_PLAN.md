# 聊天示例实现与检查

示例位于 `examples/chat`，支持 Linux x86_64 Native。服务端、客户端和共享协议均用 Woozy 编写；没有修改编译器、运行库或标准库接口。使用方法见 [聊天示例](../../examples/chat/README.md)。

## 连接和协议

两个项目清单共用 `src`，入口分别为 `app/server` 和 `app/client`。共享模块通过 `import "app/protocol" as wire` 导入。

协议使用 UTF-8 文本行，接受 LF 和 CRLF。客户端首行是昵称，其后是聊天消息；空行忽略，`/quit` 结束连接。服务端广播 `* NAME joined`、`* NAME left`、`[NAME] MESSAGE`。昵称最多 32 个码点，不允许首尾空白、控制字符或重名。

用户消息最多 16,384 字节；原始帧最多 16,640 字节，以容纳服务端添加的昵称。每连接最多积压 65,536 字节，最多 64 个连接。缓冲区跨读取保留，完整收到一行才解码 UTF-8。消息和服务器响应中的 C0/C1 控制字符不直接输出到终端；客户端收到 `ERROR ` 响应时以错误退出。

## 非阻塞读写和退出

服务端使用单线程事件循环。监听和已接收的 socket 均带 `SOCK_NONBLOCK`；`net.poll` 平时使用无限等待，有待关闭连接时改用有限等待来清理过期连接。每轮接受连接和读取的批次都有上限；慢客户端的发送队列已满或写入失败时会直接断开。

客户端同时 poll stdin 与 socket。连接最多等待 5 秒；收到 `/quit` 或 stdin EOF 后，先尝试发完队列、关闭写方向并读完剩余响应，发送和读取收尾各最多等 2 秒。服务端处理对端 EOF 时，同样给已有输出最多 2 秒发送时间。即使 stdin 仍打开，服务端返回 EOF 后客户端也会退出。

Linux accept4 会直接返回新连接上已经发生的网络错误。遇到这类临时错误时，应用跳过该连接并继续等待新连接，不中断现有会话；监听描述符本身的错误仍会抛出。处理规则依据 [Linux accept(2)](https://man7.org/linux/man-pages/man2/accept.2.html)：临时错误为 ECONNABORTED、ENETDOWN、EPROTO、ENOPROTOOPT、EHOSTDOWN、ENONET、EHOSTUNREACH、EOPNOTSUPP、ENETUNREACH。

## 实际检查

`native.chat` 在 O0 和 O2 下构建并运行服务端与客户端，共 67 项检查：

- O0：33 项；O2：34 项。
- 两个真实 Woozy 客户端互发中文和 emoji，核对发送者回显、广播、空行、CRLF、EOF、重连和昵称复用。
- 原始 TCP 客户端逐字节发送 UTF-8，覆盖分包、合并消息、消息长度上限和非法输入。
- 独立测试服务器只用于检查客户端对不可信输入的处理，不替代真实双端测试。
- 在 O2 慢读者压力测试中，慢连接会被移除，健康连接仍能收到后续消息；O0 不跑这项压力测试。
- 每个配置模拟 9 个 accept 临时错误和 2 个致命错误：通过链接器 `--wrap` 让运行库的第 3 次 accept 返回指定错误，并要求 stderr 中的标记恰好出现一次，以确认模拟确实触发。错误在已有客户端注册后注入，临时错误不影响双向聊天，错误 9/22 仍会让服务端退出。这项测试只模拟运行库返回值，没有制造真实内核网络故障。
- 生成 ELF 没有 INTERP、NEEDED 或强未定义符号；测试创建的进程和 socket 均清理。

测试 runner 是 `tests/native/run_chat.py`，额外错误模拟文件是 `tests/native/chat_accept_error.cpp`。在已配置的 Native 构建中执行：

```sh
ctest --test-dir build/woozy-stdlib-native -R '^native.chat$' --output-on-failure
```

另有 IPv6 `::1` 上两个真实 Woozy 客户端收发中文的检查。测试结果和日志留在工作目录 checks 中，不提交生成文件。

## 未覆盖的产品功能

该示例没有 TLS、账号认证、消息持久化、离线消息、自动重连或图形界面。当前测试均在本机回环网络，未测试跨主机、长时间运行或公网部署。
