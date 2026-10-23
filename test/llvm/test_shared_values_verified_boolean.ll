; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;subterms;affine;narrow-roots;two-valued-selects;shared-values;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: subterms;affine;narrow-roots;two-valued-selects;shared-values;families=semilinear+bitwise;timeout=20000

; Values other readers keep stop no reading of a root (`shared-values`): each
; function here is a fixture of another file with every intermediate also handed
; to an opaque call, and it folds as the original does.

declare void @use32(i32)

; A truncation of a wide sum a call also reads whole: at 32 bits the sum needs
; `hi`, the sixteen bits the truncation keeps do not. The truncation is a root
; of its own, and comes out `lo + 1` at sixteen bits.
;
; CHECK-LABEL: @narrow_reader_of_a_shared_sum(
; CHECK: [[A:%[a-z0-9.]+]] = add i16 %lo, 1
; CHECK-NEXT: ret i16 [[A]]
define i16 @narrow_reader_of_a_shared_sum(i16 %hi, i16 %lo) {
entry:
  %0 = zext i16 %hi to i32
  %1 = shl nuw i32 %0, 16
  %2 = zext i16 %lo to i32
  %3 = or disjoint i32 %1, %2
  %4 = xor i32 %3, -53608448
  call void @use32(i32 %4)
  %5 = add i32 %4, 1
  call void @use32(i32 %5)
  %6 = trunc i32 %5 to i16
  ret i16 %6
}

; The add-and-shift identity read through a shift by a reader of its bit 15
; (`test_demanded_bits.ll`), every intermediate also read whole by a call: the
; reader still gets `(x & y) >> 15 & 1`.
;
; CHECK-LABEL: @and_identity_bit15_shared(
; CHECK: [[A:%[a-z0-9.]+]] = and i32 %x, %y
; CHECK-NEXT: [[H:%[a-z0-9.]+]] = lshr i32 [[A]], 15
; CHECK-NEXT: [[B:%[a-z0-9.]+]] = and i32 [[H]], 1
; CHECK: ret i32 [[B]]
define i32 @and_identity_bit15_shared(i32 %x, i32 %y) {
  %d = xor i32 %x, %y
  call void @cobra.opaque.i32(i32 %d)
  %c = and i32 %d, 1
  call void @cobra.opaque.i32(i32 %c)
  %s = add i32 %x, %y
  call void @cobra.opaque.i32(i32 %s)
  %sc = add i32 %s, %c
  call void @cobra.opaque.i32(i32 %sc)
  %avg = lshr i32 %sc, 1
  call void @cobra.opaque.i32(i32 %avg)
  %d1 = lshr i32 %d, 1
  call void @cobra.opaque.i32(i32 %d1)
  %r0 = add i32 %avg, %d1
  call void @cobra.opaque.i32(i32 %r0)
  %r = sub i32 %r0, %d
  call void @cobra.opaque.i32(i32 %r)
  %hi = lshr i32 %r, 15
  call void @cobra.opaque.i32(i32 %hi)
  %bit = and i32 %hi, 1
  ret i32 %bit
}

; A `select` whose arms agree on the low half
; (`test_select_arms_on_demanded_bits_verified.ll`), also read whole by a call:
; the masked reader reads the kept arm, the call the `select`.
;
; CHECK-LABEL: @masked_arms_shared(
; CHECK: [[S:%[a-z0-9.]+]] = select i1 %c
; CHECK: call void @cobra.opaque.i64(i64 [[S]])
; CHECK: [[R:%[a-z0-9.]+]] = and i64 %w, 4294967295
; CHECK: ret i64 [[R]]
define i64 @masked_arms_shared(i64 noundef %x, i1 %c) {
  %w = mul i64 %x, 16777619
  call void @cobra.opaque.i64(i64 %w)
  %n = mul i64 %x, -16777619
  call void @cobra.opaque.i64(i64 %n)
  %u = and i64 %n, 4294967295
  call void @cobra.opaque.i64(i64 %u)
  %t = sub i64 4294967296, %u
  call void @cobra.opaque.i64(i64 %t)
  %s = select i1 %c, i64 %t, i64 %w
  call void @cobra.opaque.i64(i64 %s)
  %r = and i64 %s, 4294967295
  ret i64 %r
}

;
; CHECK-LABEL: @xor_encoded_sum_shared(
; CHECK: [[X:%[a-z0-9.]+]] = xor i64 {{%[a-z0-9.]+}}, 7640891576956012807
; CHECK: ret i64 [[X]]
define i64 @xor_encoded_sum_shared(i64 %x, i64 %y, i64 %w) {
  %ny = xor i64 %y, -1
  call void @cobra.opaque.i64(i64 %ny)
  %s = sub i64 %ny, %w
  call void @cobra.opaque.i64(i64 %s)
  %d = add i64 %x, %s
  call void @cobra.opaque.i64(i64 %d)
  %a = add i64 %d, 7640891576956012808
  call void @cobra.opaque.i64(i64 %a)
  %t = shl i64 %d, 1
  call void @cobra.opaque.i64(i64 %t)
  %u = add i64 %t, 2
  call void @cobra.opaque.i64(i64 %u)
  %m = and i64 %u, -3164960919797526002
  call void @cobra.opaque.i64(i64 %m)
  %r = sub i64 %a, %m
  ret i64 %r
}

;
; CHECK-LABEL: @halving_with_carry_shared(
; CHECK: [[R:%[a-z0-9.]+]] = mul i64 4, %z
; CHECK: ret i64 [[R]]
define i64 @halving_with_carry_shared(i16 %v) {
  %z = zext i16 %v to i64
  call void @cobra.opaque.i64(i64 %z)
  %twice = shl nuw nsw i64 %z, 1
  call void @cobra.opaque.i64(i64 %twice)
  %neg = mul nsw i64 %z, -4
  call void @cobra.opaque.i64(i64 %neg)
  %half = lshr exact i64 %neg, 1
  call void @cobra.opaque.i64(i64 %half)
  %zero = icmp eq i16 %v, 0
  call void @cobra.opaque.i1(i1 %zero)
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  call void @cobra.opaque.i64(i64 %carry)
  %top = or disjoint i64 %half, %carry
  call void @cobra.opaque.i64(i64 %top)
  %r = sub i64 %twice, %top
  ret i64 %r
}

;
; CHECK-LABEL: @sum_with_a_two_valued_term_shared(
; CHECK: [[P:%[a-z0-9.]+]] = select i1 {{%[a-z0-9.]+}}, i64 194, i64 0
; CHECK: [[S:%[a-z0-9.]+]] = add i64 %x, [[P]]
; CHECK: ret i64 [[S]]
define i64 @sum_with_a_two_valued_term_shared(i64 %x, i64 %t) {
  %u = or i64 %t, -2
  call void @cobra.opaque.i64(i64 %u)
  %v = add i64 %u, 2
  call void @cobra.opaque.i64(i64 %v)
  %w = mul i64 %v, 194
  call void @cobra.opaque.i64(i64 %w)
  %a = xor i64 %x, %w
  call void @cobra.opaque.i64(i64 %a)
  %b = and i64 %x, %w
  call void @cobra.opaque.i64(i64 %b)
  %c = shl i64 %b, 1
  call void @cobra.opaque.i64(i64 %c)
  %r = add i64 %a, %c
  ret i64 %r
}

;
; CHECK-LABEL: @flag_of_a_wide_comparison_shared(
; CHECK: ret i8 0
define i8 @flag_of_a_wide_comparison_shared(i8 %v) {
  %zero = icmp eq i8 %v, 0
  call void @cobra.opaque.i1(i1 %zero)
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  call void @cobra.opaque.i64(i64 %carry)
  %some = icmp ne i8 %v, 0
  call void @cobra.opaque.i1(i1 %some)
  %a = select i1 %some, i64 -13, i64 -12
  call void @cobra.opaque.i64(i64 %a)
  %s1 = add i64 %carry, %a
  call void @cobra.opaque.i64(i64 %s1)
  %b = select i1 %some, i64 9223372036854775795, i64 12
  call void @cobra.opaque.i64(i64 %b)
  %s2 = add i64 %s1, %b
  call void @cobra.opaque.i64(i64 %s2)
  %s3 = add i64 %s2, 4660
  call void @cobra.opaque.i64(i64 %s3)
  %above = icmp ugt i64 %s3, -11
  call void @cobra.opaque.i1(i1 %above)
  %flag = zext i1 %above to i8
  ret i8 %flag
}

;
; CHECK-LABEL: @all_but_the_sign_shared(
; CHECK: [[M:%[a-z0-9.]+]] = and i64 %x, 9223372036854775807
; CHECK-NEXT: [[R:%[a-z0-9.]+]] = icmp ne i64 [[M]], 0
; CHECK: ret i1 [[R]]
define i1 @all_but_the_sign_shared(i64 %x) {
  %d = add i64 %x, -1
  call void @cobra.opaque.i64(i64 %d)
  %l = and i64 %x, %d
  call void @cobra.opaque.i64(i64 %l)
  %r = icmp sgt i64 %x, %l
  ret i1 %r
}

declare void @cobra.opaque.i1(i1)
declare void @cobra.opaque.i32(i32)
declare void @cobra.opaque.i64(i64)
declare void @cobra.opaque.i8(i8)
