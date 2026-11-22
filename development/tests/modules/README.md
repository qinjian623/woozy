# T03 项目载入交接

`project.hpp` 按原字节复制自 integrator 发布文件，没有改接口。
`load_project` 读取项目与模块图，`resolve_project` 负责包内声明、文件别名、名称/种类、作用域、可见性和常量环；两者共用 `project.hpp` / `resolve.hpp`。实例成员与 trait 选择留给 T04。CLI 全量 check、类型检查和代码生成尚未实现。

## 读取规则

- 所有清单相对路径相对于该清单目录。`app` 指该项目的 `package.source_root`，入口采用 `package.entry`（app 或 app 子模块）。
- 一个模块只读取目录直接包含的全部 `.wz`，按文件名排序，不递归枚举子目录。子目录仅由逻辑 import 请求。
- source dependency 的 path 指项目目录；有 `lang.toml` 时读取其 source_root 和 dependencies，否则显式 path 本身就是源码根，其上下文没有额外 dependencies。依赖项目内部的 app 指依赖项目自己的源码根。
- canonical 目录是源码模块身份；同目录不同别名返回同一个从 0 开始的 ModuleId。首次按确定的 DFS 顺序分配，`modules[id.value]` 可直接读取。不同来源请求同一个 logical 名报 E_MODULE_CONFLICT。
- std 只接受 ProjectOptions 中显式源码根或 binding bundle，两者同时提供报冲突；dependencies 禁止声明 app/std。未配置 std 报 E_IMPORT_NOT_FOUND。
- import 保存文件索引、AST 节点、目标、别名和原始 span。省略别名用最后一段。保留重复 alias，交名称解析器处理。
- import 只允许由 ASCII 标识符组成的非空逻辑段，不接受绝对路径、反斜线、点段或空段。
- Python bundle 用 json-c 严格解析，只开放已声明依赖前缀或显式 std 下、bundle 中匹配 logical_path 的模块。保存原 JSON，bundle 路径按 canonical 去重。普通导出取 types/values/callables；owner_type 和 method 排除，同名 constructor 附着到 type。完整签名、宿主真实性和 backend 支持由 T04/T11 后续检查。
- AST 持有不可变快照。所有诊断 span 都来自调用方的 SourceManager。JSON/TOML 内容错误使用原文本区间，没有伪造 AST。
- 导入环诊断包含完整循环名与其余 import 的 related 位置。函数引用和 const 环不在此子任务中。

## 状态

成功才返回 complete。内容错误返回 source_error (1)，IO、目录或文件缺失返回 environment_error (2)，取消返回 cancelled (130)。出错时可能返回部分 Project 供诊断使用，调用者必须先检查 success()。native ABI 依赖明确报 E_CAPABILITY_UNAVAILABLE，等待 T11；不搜索系统目录，不安装或执行依赖。

## 实测

GNU、Clang 的 C++20 Release 测试均使用显式检查，NDEBUG 不会删除检查。测试建立并读取真实临时目录，成功后自行清理。覆盖多文件及排序、pub AST 数据、source 项目、canonical 去重、稳定 ID、声明式 JSON 导出及构造器合并、私有 bundle 模块不开放、显式 std、包名不一致、完整导入环、非法 import、缺失目录/bundle、JSON/语法错误、位置 JSON 格式化及取消。

```sh
g++ -std=c++20 -O2 -DNDEBUG -Iinclude tests/modules/project_test.cpp src/modules/project.cpp src/syntax/parser.cpp src/syntax/lexer.cpp src/support/source.cpp src/support/input.cpp src/support/diagnostic.cpp -ljson-c -o /tmp/t03-project-test
/tmp/t03-project-test
clang++ -std=c++20 -O2 -DNDEBUG -Iinclude tests/modules/project_test.cpp src/modules/project.cpp src/syntax/parser.cpp src/syntax/lexer.cpp src/support/source.cpp src/support/input.cpp src/support/diagnostic.cpp -ljson-c -o /tmp/t03-project-test-clang
/tmp/t03-project-test-clang
```

## 集成测试

构建后执行 ctest --test-dir build/frontend-debug --output-on-failure。

- modules.project：真实临时项目、TOML/JSON、std/source 依赖、身份去重、循环、错误分类、NUL 与取消。
- modules.names：词法作用域、最近绑定 kind、文件 alias、公开接口类型、非捕获函数、常量图和 3001 个常量的长依赖。
- modules.cases：独立准备的 20 个项目，由 C++ load_project + resolve_project 执行；错误必须对应规则并指向真实源文件。

单独运行：

```sh
python3 tests/modules/run_cases.py build/frontend-debug/lang_module_check
python3 tools/validate_module_fixtures.py --woozyc build/frontend-debug/woozyc
python3 tools/validate_module_fixtures.py --self-test
```

局部新绑定在初始化完成后才可见；同名初始化表达式使用外层绑定，没有外层名称则报错。公开 struct 的私有字段布局/访问、外部绑定完整签名和类型成员检查由 T04 继续处理。测试成功不表示生成了源语言可执行程序。
