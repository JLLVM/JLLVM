; RUN: jasmin %s -d %t
; RUN: jllvm -Xint %t/Test.class | FileCheck %s

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(Ljava/lang/String;)V
.end method

.method public static main([Ljava/lang/String;)V
.limit locals 1
.limit stack 3
    ; If GC were to not work, the second allocation would take place of the first, making the print below fail.
    new Test
    new java/lang/Object
    dup
    invokespecial java/lang/Object.<init>()V
    swap
    dup
    invokespecial Test.<init>()V
    ; CHECK: Test@{{.*}}
    ; CHECK: java.lang.Object@{{.*}}
    invokevirtual java/lang/Object.toString()Ljava/lang/String;
    invokestatic Test.print(Ljava/lang/String;)V
    invokevirtual java/lang/Object.toString()Ljava/lang/String;
    invokestatic Test.print(Ljava/lang/String;)V
    return
.end method
