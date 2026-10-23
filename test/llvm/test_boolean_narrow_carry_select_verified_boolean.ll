; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear+bitwise;timeout=20000

; An add spelled through halvings: `base + r`, with `r` a 32-bit value read
; zero-extended, written as `base - (~r >> 1) + ((r - 1) >> 1)` plus the carry
; into the top bit, which the program picks by testing the narrow `r` against
; zero - `select (r == 0), 0, 2^63`. Every other reading of the root holds that
; `select` as a free leaf, and a free top bit is no function of `r`; read with
; the comparison taken in, the halvings and the carry cancel to `base + zext r`.
; A rewrite that compares nothing is what the narrow comparison's cut may take,
; so the `icmp` the function holds stays the only one.
;
; CHECK-LABEL: @export_address(
; CHECK: %r = zext i32 %rva to i64
; CHECK-NEXT: [[SUM:%[a-z0-9.]+]] = add i64 {{(%base, %r|%r, %base)}}
; CHECK-NEXT: ret i64 [[SUM]]
define i64 @export_address(i64 %base, i32 %rva) {
  %r = zext i32 %rva to i64
  %not = xor i64 %r, -1
  %high = lshr i64 %not, 1
  %less = add nsw i64 %r, -1
  %zero = icmp eq i32 %rva, 0
  %low = lshr i64 %less, 1
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  %a = add i64 %carry, %base
  %b = sub i64 %a, %high
  %c = add i64 %b, %low
  ret i64 %c
}

; The carry picked by testing another narrow value: the root is `base + zext r`
; less the top bit when `h` is not zero and `r` is, or plus it the other way
; round - a function of both comparisons, which no rewrite that compares nothing
; computes. The root stays as written, and no comparison is spelled anew at 64
; bits.
;
; CHECK-LABEL: @carry_of_another_value(
; CHECK: %zero = icmp eq i32 %h, 0
; CHECK-NOT: icmp {{[a-z]+}} i64
; CHECK: ret i64
define i64 @carry_of_another_value(i64 %base, i32 %rva, i32 %h) {
  %r = zext i32 %rva to i64
  %not = xor i64 %r, -1
  %high = lshr i64 %not, 1
  %less = add nsw i64 %r, -1
  %zero = icmp eq i32 %h, 0
  %low = lshr i64 %less, 1
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  %a = add i64 %carry, %base
  %b = sub i64 %a, %high
  %c = add i64 %b, %low
  ret i64 %c
}
