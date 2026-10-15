; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=20000

; A narrow comparison the function shares stays the value it is. `%c` decides a
; branch elsewhere and the `select`s here: a pass that rewrites the uses a decided
; edge dominates finds them by that very value. The root below is `c ? 0 : -1`, and
; the boolean reading can prove it - but only by reading `%c` as its operands at
; the tree width, and restored from that reading the condition comes back as a new
; `icmp ult i64 ..., zext %h`: equal to `%c`, and a different value, which no pass
; that matches conditions by identity relates to it any more. So a `select` on a
; narrow comparison is not what sends a root to the boolean reading; the exact
; reading of narrow comparisons only answers the slice question, where nothing is
; rebuilt out of it.
;
; CHECK-LABEL: @shared_condition(
; CHECK: %c = icmp ugt i32 %h, 1010843647
; CHECK-NOT: icmp {{[a-z]+}} i64
; CHECK: ret i64
define i64 @shared_condition(i32 noundef %h) {
  %c = icmp ugt i32 %h, 1010843647
  %s0 = select i1 %c, i64 0, i64 65535
  %f1 = call i64 @llvm.fshl.i64(i64 %s0, i64 %s0, i64 59)
  %f = call i64 @llvm.fshl.i64(i64 %f1, i64 %f1, i64 9)
  %s1 = select i1 %c, i64 795, i64 4294966500
  %a = and i64 %s1, 65535
  %b = xor i64 %a, 795
  %d = add nsw i64 %b, -2
  %e = or i64 %s1, -2
  %g = shl nuw nsw i64 %f, 28
  %s2 = select i1 %c, i64 -1, i64 281474976710655
  %i = xor i64 %g, %s2
  %j = lshr i64 %i, 1
  %k = sub nsw i64 %g, %s2
  %l = lshr i64 %k, 1
  %m = sub nsw i64 %j, %l
  %n = xor i64 %m, -9223372036854775808
  %s3 = select i1 %c, i64 -1, i64 -4294901761
  %o = and i64 %n, %s3
  %p = add nsw i64 %e, %o
  %q = sub i64 %d, %p
  %r = lshr i64 %q, 1
  %s4 = select i1 %c, i64 0, i64 -9223372036854775808
  %t = or disjoint i64 %r, %s4
  %u = xor i64 %o, %a
  %v = lshr i64 %u, 1
  %w = xor i64 %v, 9223372036854775410
  %x = add i64 %t, %w
  ret i64 %x
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
