; RUN: jasmin %s -d %t
; RUN: jllvm -Xint %t/Test.class | FileCheck %s

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(I)V
.end method

.method public static main([Ljava/lang/String;)V
.limit locals 1
.limit stack 1
    getstatic java/lang/Integer/MIN_VALUE I
    ineg
    ; CHECK: -2147483648
    invokestatic Test.print(I)V
    return
.end method
