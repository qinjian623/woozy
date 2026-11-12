# std/box

`Box[T]` 保存一个可修改的值。复制 Box 后，两个变量共享同一个单元。

```wz
import "std/box" as box;

let cell = box.new[i32](1);
let alias = cell;
box.set(alias, 2);
// box.get(cell) == 2
```

公开接口只有这三个函数，Box 的字段不公开：

```wz
pub func new[T](value T) Box[T]
pub func get[T](value Box[T]) T
pub func set[T](value Box[T], next T)
```

`new` 和 `set` 按普通规则复制传入值。`get` 返回当前值的普通副本；Vec 等引用对象仍共享底层对象。`set` 会清理单元原先保存的值。给一个 Box 变量重新赋值，只改变该变量指向的单元，其他别名继续指向原来的单元。

跨线程修改同一个 Box 时，程序需要用 Mutex 等锁保护访问。

Native 暂未自动回收强引用循环，Box 循环可能一直占用内存。Python 后端可以回收纯 Woozy 对象构成的循环；外部对象仍由对应宿主管理。
