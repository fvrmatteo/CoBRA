; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Regression test: a candidate may contain narrow leaves that were widened
; through zext/sext in the original IR. Reconstructing the simplified AST must
; cast those leaves back to the candidate bitwidth before rebuilding ops.
;
; CHECK-LABEL: @MBA5(
; CHECK: ret i64

define i64 @MBA5(ptr noalias nocapture noundef nonnull readonly align 16 dereferenceable(3504) %incoming, i64 noundef %address, ptr noalias noundef %memory, ptr noalias nocapture noundef %xax, ptr noalias nocapture noundef %xbx, ptr noalias nocapture noundef %xcx, ptr noalias nocapture noundef %xdx, ptr noalias nocapture noundef %xdi, ptr noalias nocapture noundef %xsi, ptr noalias nocapture noundef %xbp, ptr noalias nocapture noundef %xsp, ptr noalias nocapture noundef %xip, ptr noalias nocapture noundef %r8, ptr noalias nocapture noundef %r9, ptr noalias nocapture noundef %r10, ptr noalias nocapture noundef %r11, ptr noalias nocapture noundef %r12, ptr noalias nocapture noundef %r13, ptr noalias nocapture noundef %r14, ptr noalias nocapture noundef %r15, ptr noalias nocapture noundef %cs, ptr noalias nocapture noundef %ds, ptr noalias nocapture readnone %fs, ptr noalias nocapture noundef %gs, ptr noalias nocapture noundef %ss, ptr noalias nocapture noundef %af, ptr noalias nocapture noundef %cf, ptr noalias nocapture noundef %df, ptr noalias nocapture noundef %of, ptr noalias nocapture noundef %pf, ptr noalias nocapture noundef %sf, ptr noalias nocapture noundef %zf, ptr noalias nocapture noundef %xmm0, ptr noalias nocapture noundef %xmm1, ptr noalias nocapture noundef %xmm2, ptr noalias nocapture noundef %xmm3, ptr noalias nocapture noundef %xmm4, ptr noalias nocapture noundef %xmm5, ptr noalias nocapture noundef %xmm6, ptr noalias nocapture noundef %xmm7, ptr noalias nocapture noundef %xmm8, ptr noalias nocapture noundef %xmm9, ptr noalias nocapture noundef %xmm10, ptr noalias nocapture noundef %xmm11, ptr noalias nocapture noundef %xmm12, ptr noalias nocapture noundef %xmm13, ptr noalias nocapture noundef %xmm14, ptr noalias nocapture noundef %xmm15) {
entry:
  %1 = load i64, ptr %xcx, align 1
  %2 = and i64 %1, 4294967294
  %3 = xor i64 %2, 2648312627
  %4 = shl i64 %1, 1
  %5 = and i64 %4, 4294966548
  %6 = xor i64 %5, 1001658094
  %7 = sub nsw i64 %6, %3
  %8 = load i64, ptr %xdx, align 1
  %9 = xor i64 %8, 20532634
  %10 = add i64 %9, 2777605667
  %11 = and i64 %7, %10
  %12 = load i64, ptr %xsi, align 1
  %13 = trunc i64 %11 to i32
  %14 = add i32 %13, 20
  %15 = zext i32 %14 to i64
  %16 = add i64 %12, %15
  %17 = load i32, ptr %xdi, align 1
  %18 = ashr i32 %17, 31
  %19 = zext i32 %18 to i64
  %20 = shl nuw i64 %19, 32
  %21 = zext i32 %17 to i64
  %22 = or disjoint i64 %20, %21
  %23 = xor i64 %22, %12
  %24 = or i64 %22, %12
  %25 = shl i64 %24, 1
  %26 = sub i64 %25, %23
  %27 = sub i64 %26, %16
  ret i64 %27
}
