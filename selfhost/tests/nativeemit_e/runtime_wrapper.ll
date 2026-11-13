; Test-only ABI adapters.  The generated functions pass/return large LLVM
; aggregates by value, while the C driver uses the platform C ABI.  These
; wrappers have scalar/pointer C-compatible signatures and call the exact
; generated function bodies.  They are not part of the production module.
%"value.9" = type { i32, {}, { { ptr, i64, i64, ptr } } }
%"value.10" = type { i32, { %"value.9" }, { i32 } }
%"value.11" = type { i32, {}, { ptr } }
%"value.12" = type { i32, { %"value.11" }, { i32 } }
%"c.record.1" = type { i8, i32 }
%"c.record.2" = type { %"c.record.1", ptr }

declare %"value.10" @"ex_getenv_probe"({ ptr, i64, i64, ptr })
declare %"value.12" @"ex_getenv_bytes_probe"(ptr)
declare i1 @"ex_field_bool"(%"c.record.1")
declare i32 @"ex_field_i32"(%"c.record.1")
declare %"c.record.1" @"ex_field_nested"(%"c.record.2")
declare ptr @"ex_field_pointer"(%"c.record.2")

define i32 @wrap_getenv(ptr %data, i64 %len, ptr %out) {
entry:
  %a0 = insertvalue { ptr, i64, i64, ptr } zeroinitializer, ptr %data, 0
  %a1 = insertvalue { ptr, i64, i64, ptr } %a0, i64 %len, 1
  %a2 = insertvalue { ptr, i64, i64, ptr } %a1, i64 %len, 2
  %res = call %"value.10" @"ex_getenv_probe"({ ptr, i64, i64, ptr } %a2)
  %rtag = extractvalue %"value.10" %res, 0
  %iserror = icmp eq i32 %rtag, 1
  br i1 %iserror, label %error, label %ok
error:
  %code = extractvalue %"value.10" %res, 2, 0
  ret i32 %code
ok:
  %opt = extractvalue %"value.10" %res, 1, 0
  %otag = extractvalue %"value.9" %opt, 0
  %some = icmp eq i32 %otag, 1
  br i1 %some, label %somebody, label %none
somebody:
  %payload = extractvalue %"value.9" %opt, 2, 0
  store { ptr, i64, i64, ptr } %payload, ptr %out
  ret i32 1
none:
  ret i32 0
}

define i32 @wrap_getenv_bytes(ptr %name, ptr %out) {
entry:
  %res = call %"value.12" @"ex_getenv_bytes_probe"(ptr %name)
  %rtag = extractvalue %"value.12" %res, 0
  %iserror = icmp eq i32 %rtag, 1
  br i1 %iserror, label %error, label %ok
error:
  %code = extractvalue %"value.12" %res, 2, 0
  ret i32 %code
ok:
  %opt = extractvalue %"value.12" %res, 1, 0
  %otag = extractvalue %"value.11" %opt, 0
  %some = icmp eq i32 %otag, 1
  br i1 %some, label %somebody, label %none
somebody:
  %payload = extractvalue %"value.11" %opt, 2, 0
  store ptr %payload, ptr %out
  ret i32 1
none:
  ret i32 0
}

define i32 @wrap_field_bool(i8 %flag, i32 %value) {
entry:
  %p0 = insertvalue %"c.record.1" zeroinitializer, i8 %flag, 0
  %p1 = insertvalue %"c.record.1" %p0, i32 %value, 1
  %result = call i1 @"ex_field_bool"(%"c.record.1" %p1)
  %wide = zext i1 %result to i32
  ret i32 %wide
}

define i32 @wrap_field_i32(i8 %flag, i32 %value) {
entry:
  %p0 = insertvalue %"c.record.1" zeroinitializer, i8 %flag, 0
  %p1 = insertvalue %"c.record.1" %p0, i32 %value, 1
  %result = call i32 @"ex_field_i32"(%"c.record.1" %p1)
  ret i32 %result
}

define i32 @wrap_field_nested(i8 %flag, i32 %value, ptr %pointer) {
entry:
  %p0 = insertvalue %"c.record.1" zeroinitializer, i8 %flag, 0
  %inner = insertvalue %"c.record.1" %p0, i32 %value, 1
  %n0 = insertvalue %"c.record.2" zeroinitializer, %"c.record.1" %inner, 0
  %n1 = insertvalue %"c.record.2" %n0, ptr %pointer, 1
  %result = call %"c.record.1" @"ex_field_nested"(%"c.record.2" %n1)
  %rflag = extractvalue %"c.record.1" %result, 0
  %rvalue = extractvalue %"c.record.1" %result, 1
  %wide = zext i8 %rflag to i32
  %encoded = add i32 %rvalue, %wide
  ret i32 %encoded
}

define i32 @wrap_field_pointer(i8 %flag, i32 %value, ptr %pointer) {
entry:
  %p0 = insertvalue %"c.record.1" zeroinitializer, i8 %flag, 0
  %inner = insertvalue %"c.record.1" %p0, i32 %value, 1
  %n0 = insertvalue %"c.record.2" zeroinitializer, %"c.record.1" %inner, 0
  %n1 = insertvalue %"c.record.2" %n0, ptr %pointer, 1
  %result = call ptr @"ex_field_pointer"(%"c.record.2" %n1)
  %same = icmp eq ptr %result, %pointer
  %wide = zext i1 %same to i32
  ret i32 %wide
}
