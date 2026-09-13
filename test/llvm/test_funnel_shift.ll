; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; A rotate lifts to a funnel shift by a constant, and an obfuscator's identity
; can run through it. Here the guest's thread-local block address is rebuilt
; from itself and from a value the protector derives from the TLS slot's own
; address: that value is `(a ^ k) & (a ^ ~k)` - zero for every `a` - written
; behind `fshl`, a truncation and the add-and-shift identity
; `((x + y + ((x ^ y) & 1)) >> 1) + ((x ^ y) >> 1) == x | y`, and once it is
; zero every constant in the rest cancels, leaving the block pointer plus a
; displacement. With the funnel shift left as an opaque leaf the value under it
; is cut off from the copy read through the other truncation, and the zero is
; not provable; modelled as the shift it is, the whole chain is one variable.

; CHECK-LABEL: @tls_displacement(
; CHECK-NOT: call i32 @llvm.fshl
; CHECK: %[[R:.*]] = add i64 %block, 1016
; CHECK-NEXT: ret i64 %[[R]]
define i64 @tls_displacement(i64 %slot, i64 %block) {
  %s = xor i64 %slot, -3316038060006119497
  %hi = lshr i64 %s, 32
  %h16 = trunc i64 %hi to i16
  %h16x = xor i16 %h16, 26720
  %z = zext i16 %h16x to i32
  %rot = tail call i32 @llvm.fshl.i32(i32 %z, i32 4244, i32 16)
  %h32 = trunc nuw i64 %hi to i32
  %y = xor i32 %h32, 4340
  %p = lshr exact i32 %rot, 16
  %p2 = xor i32 %p, 34667
  %x = or disjoint i32 %p2, -65536
  %d = xor i32 %x, %y
  %d1 = lshr i32 %d, 1
  %c0 = xor i32 %p2, %h32
  %c = and i32 %c0, 1
  %sum = add i32 %x, %y
  %sumc = add i32 %sum, %c
  %avg = lshr i32 %sumc, 1
  %r = add nuw i32 %avg, %d1
  %q = xor i32 %r, %y
  %m = and i32 %q, 65535
  %v = xor i32 %m, %p2
  %vz = zext nneg i32 %v to i64
  %vm = shl nuw nsw i64 %vz, 32
  %bk = xor i64 %block, 4321605039284771879
  %top = and i64 %block, -281474976710656
  %top2 = xor i64 %top, -3316056700628238336
  %lo = and i64 %block, 4294967295
  %mid = and i64 %bk, 281470681743360
  %mid2 = xor i64 %vm, %mid
  %joined = or disjoint i64 %mid2, %lo
  %j2 = xor i64 %joined, 136434395178935
  %j3 = add i64 %j2, %top2
  %j4 = xor i64 %j3, -3316038060006119497
  %addr = add i64 %j4, 1016
  ret i64 %addr
}

declare i32 @llvm.fshl.i32(i32, i32, i32)
