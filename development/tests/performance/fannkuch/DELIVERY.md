# Fannkuch-redux 准备交付（correction 1/1）

This file records pre-release measurements made with the old `langc` / `.wz` names. The archived hashes and observations below retain those names. Current releases use `woozyc`, `.wz`, and the runner option `--woozyc`; see the current [performance summary](../../../docs/PERFORMANCE.md).

任务：`language-classic-fannkuch-prepare-20261109`
契约版本：`compiler-dev/1`
状态：`done`（仅正确性与编译产物准备；正式计时 `not_run`）

本文件续接被退回的 task74bf6eb0，不新增扩展任务。已按
`<dev-work-dir>/checks/fannkuch-first-delivery-review-20261109.md`
修正真实 Source 观察器、实际 visit 符号识别、Source 负控制和公共准备代码复用。
没有提交、推送或另派 agent；未修改主仓、冻结 oracle、公共 runtime 或其他算法。

## 本轮修正

1. `visit_trace.py` 不再是 Python 算法模型。`prepare.py` 用实际
   `lang_python_emit` 生成未改写的 `program.py`、`langrt.py`、`verified.hir`、
   `metadata.json`；观察器 import 生成的 `program.py`，按 metadata 找到
   `visit` 和 `main`，包装 `visit` 记录实际收到的 `index`、`values`、`flips`，
   再调用原空 `visit`，最后完整执行 `main`。观察器不重算 Fannkuch、排列或翻转。
2. Source/C++ 的 `visit` 增加实际排列快照参数：
   `visit(index u64, values Vec[i32], flips i32)` 和
   `static void visit(uint64_t, const std::vector<int32_t>&, int32_t)`。
   调用传入当时的 `perm1`。
3. Source visit 调用改用 metadata 中的真实 Source `visit` symbol ID，并核对它
   在 NativeIR 中为 `function @<id>`、在 LLVM 中为 `@lang.fn.<id>`，再统计
   指向该符号的实际 `call`/`invoke`。没有硬编码 21，也不按 `visit` 单词搜索。
4. C++ visit 调用由 O0 IR 的 debug metadata
   `DISubprogram(name: "visit", linkageName: ...)` 识别 mangled 符号，再统计该
   mangled 符号的实际调用。O0 必须正计数，O2 必须为 0。
5. 新增两个真实 Source 负控制，均保持 checksum/max 检查和 generated `main`
   正常退出 0：
   - 只把传给 `visit` 的 flips 改成 `flips + 1`；真实观察器必须失败于 `flips`。
   - 只把传给 `visit` 的排列改成翻转后的 `perm`；真实观察器必须失败于
     `permutation`。
6. `prepare.py` 删掉自写的构建树和工具识别代码，直接复用 `run_native.py` 的
   `common.base_report`、`build_tree_info`、工具 SHA/版本检查。当前脚本 828 行，
   减少了 Release 宏匹配的重复实现；本轮没有新增测量框架或正式计时。

## 输入与未改材料

- 源码基线：worktree HEAD `36555eba3cb81790662965d172fb29189b2a33a1`（`36555eb`）；
  相对 `163ba5c` 只有三份文档变化，编译器代码相同。
- 主仓：`<repo>`，HEAD 相同。运行前后
  未修改主仓文件；只保留原有未跟踪项 `not a tty`。
- 本轮使用单独的 production Release 构建：
  `build/native-perf-release`，
  `CMAKE_BUILD_TYPE=Release`，同一 `36555eb` 源码，`BUILD_TESTING=OFF`。
  该构建的四个 `LANG_NATIVE_*` 定义均唯一，公共 `run_native.py` 检查通过，
  没有放宽宏筛选。
- `lang_python_emit`：
  `build/native-cli-release/tests/python/lang_python_emit`
  同版本的稳定构建；SHA 已记录。
- 冻结 oracle：
  `<dev-work-dir>/checks/fannkuch-oracle-20261109`。
  `results.json`、`inputs.json`、5 份 trace、5 份 expected-output 共 12 份文件
  在运行前后 SHA-256 完全一致。n3..7 trace 为 6/24/120/720/5040 行，共 5910 行。
- 固定 LLVM：`${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8`。
- 固定 CPython：`${WOOZY_TOOLCHAIN_ROOT}/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12`。
- 运行环境：`LD_LIBRARY_PATH=${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib`；
  子进程固定到 CPU 4。
- 原 r1..r6 运行日志仍保留（`/tmp/fannkuch-prepare-run-r1..r6.log`），r6 构建目录未改；本次新输出使用独立目录，不覆盖 r6。

## 变更文件

仅修改/保留 `tests/performance/fannkuch/` 现有 6 个文件：

| 文件 | 内容 |
| --- | --- |
| `fannkuch.wz.in` | Source 模板；`visit(index, values, flips)`、实际 `perm1` 快照和可渲染正确/错误期望 |
| `reference.cpp.in` | C++20 模板；对应 `visit` 签名和相同占位符 |
| `visit_trace.py` | import 未改写生成 Python 模块的真实 observer |
| `prepare.py` | 复用公共 run_native 的准备脚本；真实 trace、负控制、符号身份和 LLVM/对象检查 |
| `README.md` | 使用说明 |
| `DELIVERY.md` | 本交付记录 |

Source 与 C++ 继续使用相同的单线程迭代 count 数组生成顺序（官方递归前缀左旋
顺序的等价增量生成）、相同前缀翻转、逐元素 copy 和按官方零基序号交替 checksum。
三个工作数组只在每次完整 `fannkuch(n)` 调用开始时分配，排列之间不重新分配；
没有 SIMD、多线程或网站优化版本。

## 已支持

- n=3..7 的 Source Native O0/O2 与 C++ O0/O2 正确执行：20/20，安静退出 0。
- 错 checksum：20/20，安静退出 1；错 maximum：20/20，安静退出 2。
- 两个 Source 负控制都经 Source O0/O2 实际构建和执行，共 20/20 安静退出 0，
  即 checksum/max 和 `main` 正常；Python generated `main` 也正常退出 0。
- 真实 Source observer：
  - 正确程序的 generated `main` 完整执行，5 份真实 trace 与冻结 trace 逐字节一致。
  - 5910/5910 行一致；n3..7 派生 checksum/max 为
    `2/2`、`4/4`、`11/7`、`49/10`、`228/16`。
  - 错 flips 负控制 5/5 被拒绝，首个差异字段均为 `flips`。
  - 错排列负控制 5/5 被拒绝，首个差异字段均为 `permutation`。
- Source visit 身份与调用：
  - 每个 n 的 metadata 都给出一个 Source `visit` symbol（本轮均为 21，但由 metadata
    读取，不硬编码）；同一 symbol 在 NativeIR 中为 `function @21`，在 LLVM 中为
    `@lang.fn.21`。
  - O0 before/after 都识别到 1 个实际调用，正控制 5/5 通过。
  - O2 before 识别到 1 个实际调用，after 为 0；O2 零控制 5/5 通过。
- C++ visit 身份与调用：
  - 从 O0 IR debug metadata 识别 mangled linkage
    `_ZL5visitmRKSt6vectorIiSaIiEEi`。
  - O0 每个 n 识别到 1 个 `invoke`，正控制 5/5；O2 每个 n 为 0，零控制 5/5。
- LLVM/对象：
  - 10 份 Source LLVM export；Source before/after verifier 20/20 通过。
  - helper 对象与正式 `langc program.o` 10/10 字节一致。
  - C++ O0/O2 IR 10/10 verifier 通过。

## 明确拒绝或未支持

- 不运行正式计时、吞吐、加速比或 `<=2x` 比较；本轮没有性能结论。
- 不覆盖 n=8 或更大输入。
- 不使用网站 SIMD/多线程优化版本。
- 负控制观察器只针对 Source；C++ 侧另用 mangled 符号 O0 正计数/O2 零计数检查。
- 不覆盖其他目标、CPU、编译器或 LLVM 版本。

## 实际命令

最终通过运行使用新的独立输出目录：

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
REPO="$(git rev-parse --show-toplevel)"
WORKTREE="${WOOZY_WORKTREE:-$REPO}"
MAIN="${WOOZY_WORK_DIR:-$(dirname "$REPO")}"
cd "$WORKTREE"
"${WOOZY_TOOLCHAIN_ROOT}/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12" \
  -B -s tests/performance/fannkuch/prepare.py \
  --out build/fannkuch-prepare-20261109-correction-r10 \
  --oracle-dir "$MAIN/checks/fannkuch-oracle-20261109" \
  --python "${WOOZY_TOOLCHAIN_ROOT}/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12" \
  --python-helper "$REPO/build/native-cli-release/tests/python/lang_python_emit" \
  --langc "$REPO/build/native-perf-release/langc" \
  --helper "$REPO/build/native-perf-release/native/lang_native_helper" \
  --runtime "$REPO/build/native-perf-release/native/liblang_native_runtime.a" \
  --llvm-bin "${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8/bin" \
  --compiler-root "$REPO" \
  --ld-library-path "${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib" \
  --cpu 4
```

退出码 0。报告：
`build/fannkuch-prepare-20261109-correction-r10/prepare.json`
SHA-256：`5b9647fff0f6836d9781d0be08d99e2d483f7f9d6a38e1449958ad9d6edbf280`

报告含 256 条完整子命令记录，每条都有 `argv/status/exit/stdout/stderr/timeout_s`：
216 条退出 0，20 条预期退出 1，20 条预期退出 2。

correction 过程中的 r7-r9 是准备脚本自身的字段或命名错误导致的失败尝试，全部保留在
独立的 `build/fannkuch-prepare-20261109-correction-r*` 目录中，没有作为通过证据；
最终通过的是 r10。

C++ 构建使用
`--target=x86_64-unknown-linux-gnu -march=x86-64 -mtune=generic`、
`-std=c++20`、`-fno-fast-math`、`-fno-lto` 和固定 `ld.lld`。C++ O0/O2 IR
另用 `-g -S -emit-llvm` 导出以识别 debug metadata 和实际 mangled 调用。

## 验证结果

最终 `prepare.json` 摘要：

```text
status: passed
cases: 5 (n=3..7)
correct executions: 20 / quiet exit 0: 20
wrong executions: 40
wrong checksum quiet exit 1: 20
wrong maximum quiet exit 2: 20
negative Source executions: 20 / quiet exit 0: 20
negative observer executions: 10 / rejections: 10
real Python visit trace lines: 5910 / matches: 5
Source LLVM exports: 10
Source IR verifies: 20
C++ IR verifies: 10
all IR verifies: 30
Source object byte matches: 10
Source O0 positive visit-call cases: 5
Source O2 zero visit-call cases: 5
Source O2 actual visit calls: 0
C++ O0 positive mangled visit-call cases: 5
C++ O2 zero mangled visit-call cases: 5
C++ O2 actual visit calls: 0
frozen oracle files unchanged: true
```

实际观察器证据：

| n | 真实 trace 行 | 冻结 trace SHA | 错 flips 首差异 | 错排列 首差异 |
| --- | --- | --- | --- | --- |
| 3 | 6 | `7f50032c...` | line 1 / flips | line 2 / permutation |
| 4 | 24 | `69c039d0...` | line 1 / flips | line 2 / permutation |
| 5 | 120 | `aa1e95c2...` | line 1 / flips | line 2 / permutation |
| 6 | 720 | `d6f8772a...` | line 1 / flips | line 2 / permutation |
| 7 | 5040 | `4f37e7ac...` | line 1 / flips | line 2 / permutation |

Source visit 符号识别与调用计数：

| n | metadata visit symbol | NativeIR | O0 before/after | O2 before/after |
| --- | --- | --- | --- | --- |
| 3 | 21 | `function @21` | 1 / 1 | 1 / 0 |
| 4 | 21 | `function @21` | 1 / 1 | 1 / 0 |
| 5 | 21 | `function @21` | 1 / 1 | 1 / 0 |
| 6 | 21 | `function @21` | 1 / 1 | 1 / 0 |
| 7 | 21 | `function @21` | 1 / 1 | 1 / 0 |

符号 21 来自每份生成 metadata 的 `source_name == "visit"` 记录，脚本没有写死
`21`；若 symbol 不存在于 NativeIR/LLVM，准备会失败。

## 工具与脚本 SHA-256

| 工具 | SHA-256 |
| --- | --- |
| production `langc` | `9daf4211de54c36d089d0ad00291bab15e27ffc0ed2465115ed983f4bc09ba41` |
| production `lang_native_helper` | `a9cf768a2426e58b531abb222608c5cef26f8c1a9bedcb539461435e987a5933` |
| production `liblang_native_runtime.a` | `f0c676c60a6982c5d0497e0f685ed58e3d57cc961e62ab9c62bf8579a1376d02` |
| `clang` / `clang++` | `31a40dc31f3c15a47aa119aa148f339bf363d8f61202ddbdacbd3f24b71ba113` |
| `ld.lld` | `a37ea59e863b4c1fd95ead6a905f7ddb461f7d90d4f30d2d76bcbb2ae9588b1d` |
| `opt` | `fc19bcd0ab81c35746c93aad355e8bb81f34ab070e98014cf06ce8433e91d4b1` |
| CPython 3.12.15 | `eaaa308032c4aa063df5880c4c293cd458b4629d503fbc2c03ec163d71f7f790` |
| `lang_python_emit` | `5b52957702197733f40b0984f9eb47183070d95f5e751c81a37f49f59dbd5e3f` |
| `run_native.py`（复用） | `110f8ed89807f2f7d137bfd0677710badf0fb67a526f48befe06f349a51e00fd` |

| 文件 | SHA-256 |
| --- | --- |
| `fannkuch.wz.in` | `3d2b2c1de4ef9894dcff3645c7fe7cf0a420ec024d46cf19b49a31e8f68028a5` |
| `reference.cpp.in` | `085dcac906d1db8d1432d8e8175c16bf6a198271c119c88ab84113cc35f75ae2` |
| `visit_trace.py` | `e3685f2d1daf0f523c56624fb6165247c3ed0d8b0d9195a5253011fbd5bfb1fe` |
| `prepare.py` | `733c2da0a63154f6a952e9d3a96162c71ce95e8cca581266f76ed2838a6d2474` |

正确程序 SHA-256：

| n | Source O0 | Source O2 | C++ O0 | C++ O2 |
| --- | --- | --- | --- | --- |
| 3 | `fef298277b22a8270d1bcb2d956f41c2e7a50dd9b7150bd7d42801b38efebaca` | `f139a8e090f951573db8f6d401bc7fc6c6aad6a0394e2de21d00316bdb4589f6` | `cd7f0674ac26118045a9d132a278ba5b51d1aaa92a8ce5cc573da7bb53c52ae9` | `399eb9e695943f87d627121960888e9b612d4dd263e8691e48c15b77486ef6b7` |
| 4 | `4f65f61c99e5b0b0a9291e0496b179a1d8239085eb75a6bb6fc98a122686d0cd` | `5d5043c764cd4dbc118836bd9959986775a1fbaba96f7536d7d8b5ca7ccc4741` | `db48f78e0213994946a6e3b7304407a85cae594d96ae90c93bbd41e3f325ea4c` | `9acdcfce72c2698b3c948ee05de19705e45bf6723de58e248a889d8b397a334a` |
| 5 | `e5ead35b539ab1b5d9501c0b77a08089103372a3ff3a0238439ed0311a3ef901` | `c746699ed263b50ca694c3b64d38f05f6a8fdb05e9ed8df45ab0f77ad1a781cf` | `7e998df1fe49e0482f2deb6a435128bf90b79e6992d8e843e526b34ca8232446` | `b4891b5131f09a6d754c5331249891c24ffb9b0a4f52440b0ea536baf6354bb7` |
| 6 | `705bec540e603f20cfc8cc78436fba9f134dd29f1ee6da891e3e94c781db6469` | `d66503a05ea62bb2b6b4854fddd43ecba579ed7bea6c92c46fc6f52da09a9863` | `e6013fcec0c46679ae5b0077a6363ec72a39bb8b1f013cc196416ad9fd15bf34` | `905a1a27bccba9a5dab300b916862d86ca7ab4d96af4428ec2ebc0df4002e073` |
| 7 | `4af711577a2429664e6f9117c1d8d002efdd94c56facd247ca4e29400b2bde47` | `332b68b756626dd0e6543ddad9691e1116ff4662b5d32b8f0d9f080aea2af426` | `93b54902f20b6b0c68af71fae7e664f201cac74b651bf28cebf8c0188b897276` | `0830c99dc487a100f269fb12a99ef0c20d2c767c132c5c72fa9828d39573fba3` |

错 checksum/maximum、负控制、helper 对象、Python 生成文件、trace 和所有子命令的
SHA/argv/exit 均在最终 `prepare.json` 中逐项记录。

## 接口变化

无公共接口变化。没有新增语言关键字、运行时入口、公共 HIR/NativeIR 字段、shared
schema 或测量框架接口。`visit` 仍是本 fixture 的空函数；Python observer 只存在
于准备目录。

## 依赖交接

下一个角色可以直接使用：

- `tests/performance/fannkuch/fannkuch.wz.in`
- `tests/performance/fannkuch/reference.cpp.in`
- `tests/performance/fannkuch/visit_trace.py`
- `tests/performance/fannkuch/prepare.py`
- 已通过的 `build/fannkuch-prepare-20261109-correction-r10/prepare.json`

正式计时尚未开始，不能把本文件当作性能或 `<=2x` 证据。计时阶段仍需单独的
idle-host 运行、明确记录进程级 wall time，并继续使用完整 checksum/max 而不是
官方 stdout 文本作为正确性判据。

## 剩余问题

1. 尚未运行 n=8 或更大输入，也没有做正式计时、冷缓存、性能计数器或
   governor/CPU isolation 控制。
2. 负控制 observer 只针对 Source；C++ 的对应检查是 IR 中 mangled 符号的
   O0 正计数/O2 零计数，不是 C++ 运行时 observer。
3. 本轮 production Release 使用 `native-perf-release`；Python 生成器使用
   `native-cli-release/tests/python/lang_python_emit`，两者均为同一 `36555eb`
   源码，但它们是不同构建目录，报告中分别记录 SHA。
