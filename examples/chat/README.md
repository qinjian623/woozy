# TCP 聊天：server 和 client

这是用 Woozy 编写的命令行聊天程序，包含服务端、客户端和共享协议模块。支持多个用户同时在线、中文和 emoji、昵称、消息广播，以及断开后重新连接。当前用于 Linux x86_64 Native。

## 编译和运行

在 Woozy 仓库根目录执行：

```sh
woozyc build --project examples/chat/server.toml --backend native --opt 2 --out-dir build/chat/server
woozyc build --project examples/chat/client.toml --backend native --opt 2 --out-dir build/chat/client
```

第一个终端启动服务端：

```sh
build/chat/server/program 127.0.0.1 9000
```

看到 `LISTENING 127.0.0.1 9000` 后，在另外两个终端分别连接：

```sh
build/chat/client/program 127.0.0.1 9000 Alice
build/chat/client/program 127.0.0.1 9000 小明
```

输入一行并回车，所有在线用户都会收到消息，包括发送者：

```text
* Alice joined
* 小明 joined
[Alice] 你好，Woozy！
[小明] 收到了 😀
```

输入 `/quit` 退出客户端，或用 Ctrl+D 结束输入。输入结束时，最后一行即使没有换行也会尝试发送。服务端断开后，客户端会退出；需要重连时重新启动客户端。Ctrl+C 停止服务端。

服务端关闭和客户端退出前会尝试发送剩余数据；客户端发送、读取结束各最多等待 2 秒，不无限等待对方。

## 地址和昵称

```text
chat-server [IP] [PORT]
chat-client [IP] [PORT] [NICKNAME]
```

省略参数时使用 `127.0.0.1:9000`，客户端昵称为 `guest`。同一服务端不能有重名用户。昵称为 1～32 个 Unicode 码点，不能带首尾空白或控制字符。

IP 参数接受数字 IPv4 或 IPv6，例如 `127.0.0.1`、`::1`，不接受域名。服务端端口为 0 时由系统选择空闲端口，并打印实际端口；客户端端口必须为 1～65535。`--help` 显示用法。

局域网使用时，服务端可以监听 `0.0.0.0`，客户端填写服务端的实际局域网 IP：

```sh
build/chat/server/program 0.0.0.0 9000
build/chat/client/program 192.168.1.20 9000 Alice
```

## 消息和错误

每行是一条 UTF-8 消息，接受 LF 或 CRLF。空行忽略；一条用户消息最多 16,384 字节。控制字符和非法 UTF-8 不作为聊天内容显示。客户端输入错误会显示提示并忽略该行，下一行可以继续发送；服务器拒绝连接或发来非法消息时，客户端显示错误并退出。

服务端最多同时接收 64 个连接，每个连接的发送队列最多 65,536 字节。发送使用非阻塞 socket；队列已满或写入失败时断开该连接，其他用户继续聊天。TCP 分包和多个消息合在一次读取中的情况由程序缓冲处理。

客户端正常退出码为 0，连接或协议错误为 1，参数错误为 2。服务端把运行错误写到标准错误。

这个示例使用明文 TCP，没有 TLS、账号认证、历史记录、离线消息或图形界面。

## 源码

- [服务端](src/server/main.wz)：连接管理、`poll`、广播和发送队列。
- [客户端](src/client/main.wz)：同时等待键盘和网络、连接超时、退出与错误处理。
- [共享协议](src/protocol/protocol.wz)：按字节处理消息分段、UTF-8 检查与队列写入。
