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

; CHECK-LABEL: define void @"Test.test:()V"
; CHECK-SAME: prefix { ptr addrspace(1), ptr } { ptr addrspace(1) @"LTest;", ptr @"&Test.test:()V" }
.method public static test()V
    .limit stack 1
    return
.end method
