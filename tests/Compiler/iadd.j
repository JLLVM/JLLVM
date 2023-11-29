; RUN: jasmin %s -d %t
; RUN: jllvm-jvmc --method "test:()V" %t/Test.class | FileCheck %s

.class public Test
.super java/lang/Object

.field public static foo Ljava/lang/Object;

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(I)V
.end method

; CHECK-LABEL: define void @"LTest;.test:()V"
.method public static test()V
    ; CHECK: %[[OP0:.*]] = alloca
    ; CHECK: %[[OP1:.*]] = alloca
    .limit stack 2
    ; CHECK: store i32 1, ptr %[[OP0]]
    iconst_1
    ; CHECK: store i32 2, ptr %[[OP1]]
    iconst_2
    ; CHECK: %[[RHS:.*]] = load i32, ptr %[[OP1]]
    ; CHECK: %[[LHS:.*]] = load i32, ptr %[[OP0]]
    ; CHECK: %[[ADD:.*]] = add i32 %[[LHS]], %[[RHS]]
    ; CHECK: store i32 %[[ADD]], ptr %[[OP0]]
    iadd
    pop
    ; CHECK: ret void
    return
.end method
