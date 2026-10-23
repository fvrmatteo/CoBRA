; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;affine;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: affine;families=semilinear+bitwise;timeout=20000

; Without `shared-values` a truncation is no root: the 32-bit sum another reader
; keeps needs `hi`, and the sixteen bits the truncation reads stay as written.
;
; CHECK-LABEL: @narrow_reader_of_a_shared_sum(
; CHECK: [[S:%[0-9]+]] = add i32 {{%[0-9]+}}, 1
; CHECK: [[T:%[0-9]+]] = trunc i32 [[S]] to i16
; CHECK-NEXT: ret i16 [[T]]
declare void @use32(i32)

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
