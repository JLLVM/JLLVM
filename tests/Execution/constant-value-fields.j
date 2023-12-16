; RUN: jasmin %s -d %t
; RUN: jllvm -Xjit %t/Test.class
; RUN: jllvm -Xint %t/Test.class

.class public Test
.super java/lang/Object

.field public static final i I = 1

.field public static final f F = 2.0f

.field public static final l J = 3000000000

.field public static final d D = 4.0d

.field public static final s Ljava/lang/String; = "5"

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(I)V
.end method

.method public static native print(F)V
.end method

.method public static native print(J)V
.end method

.method public static native print(D)V
.end method

.method public static native print(Ljava/lang/String;)V
.end method

.method public static main([Ljava/lang/String;)V
.limit stack 2
    getstatic     Test/i I
    ; CHECK: 1
    invokestatic  Test/print(I)V
    getstatic     Test/f F
    ; CHECK: 2
    invokestatic  Test/print(F)V
    getstatic     Test/l J
    ; CHECK: 3000000000
    invokestatic  Test/print(J)V
    getstatic     Test/d D
    ; CHECK: 4
    invokestatic  Test/print(D)V
    getstatic     Test/s Ljava/lang/String;
    ; CHECK: 5
    invokestatic  Test/print(Ljava/lang/String;)V
    return
.end method
