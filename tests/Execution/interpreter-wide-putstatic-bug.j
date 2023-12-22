; RUN: jasmin %s -d %t
; RUN: jllvm -Xint %t/Test.class | FileCheck %s

.class public Test
.super java/lang/Object

.field public static final d D = 4.0d
.field public static other D = 4.0d

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(D)V
.end method

.method public static main([Ljava/lang/String;)V
.limit locals 1
.limit stack 2
    getstatic Test/d D
    putstatic Test/other D
    getstatic Test/other D
    ; CHECK: 4
    invokestatic Test.print(D)V
    return
.end method
