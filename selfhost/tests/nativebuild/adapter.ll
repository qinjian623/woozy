target triple = "x86_64-unknown-linux-gnu"
declare i32 @build_adapter()
define i32 @main() {
entry:
  %answer = call i32 @build_adapter()
  ret i32 %answer
}
