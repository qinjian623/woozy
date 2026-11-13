target triple = "x86_64-unknown-linux-gnu"
%Promise = type { i32, i32, i32 }
@marker = global i32 0
declare void @publish(ptr)
declare ptr @get_handle()
declare ptr @allocate(i64)
declare void @release(ptr)
declare token @llvm.coro.id(i32, ptr, ptr, ptr)
declare i1 @llvm.coro.alloc(token)
declare i64 @llvm.coro.size.i64()
declare ptr @llvm.coro.begin(token, ptr)
declare token @llvm.coro.save(ptr)
declare i8 @llvm.coro.suspend(token, i1)
declare ptr @llvm.coro.free(token, ptr)
declare void @llvm.coro.end(ptr, i1, token)
declare void @llvm.coro.resume(ptr)
declare void @llvm.coro.destroy(ptr)
define void @coroutine() presplitcoroutine {
entry:
  %promise = alloca %Promise, align 8
  %id = call token @llvm.coro.id(i32 8, ptr %promise, ptr null, ptr null)
  %need = call i1 @llvm.coro.alloc(token %id)
  %size = call i64 @llvm.coro.size.i64()
  %allocated = call ptr @allocate(i64 %size)
  %memory = select i1 %need, ptr %allocated, ptr null
  %handle = call ptr @llvm.coro.begin(token %id, ptr %memory)
  call void @publish(ptr %handle)
  %save = call token @llvm.coro.save(ptr %handle)
  %suspended = call i8 @llvm.coro.suspend(token %save, i1 false)
  switch i8 %suspended, label %exit [i8 0, label %body i8 1, label %cleanup]
body:
  store i32 42, ptr @marker
  br label %final
final:
  %finalsave = call token @llvm.coro.save(ptr %handle)
  %finalsuspended = call i8 @llvm.coro.suspend(token %finalsave, i1 true)
  switch i8 %finalsuspended, label %exit [i8 0, label %cleanup i8 1, label %cleanup]
cleanup:
  %freememory = call ptr @llvm.coro.free(token %id, ptr %handle)
  call void @release(ptr %freememory)
  br label %exit
exit:
  call void @llvm.coro.end(ptr null, i1 false, token none)
  ret void
}
define i32 @main() {
entry:
  call void @coroutine()
  %handle = call ptr @get_handle()
  call void @llvm.coro.resume(ptr %handle)
  %answer = load i32, ptr @marker
  call void @llvm.coro.destroy(ptr %handle)
  ret i32 %answer
}
