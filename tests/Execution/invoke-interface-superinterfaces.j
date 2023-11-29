; RUN: rm -rf %t && split-file %s %t
; RUN: cd %t && jasmin %t/Test.j -d %t && jasmin %t/A.j && jasmin %t/B.j -d %t
; RUN: jllvm %t/Test.class | FileCheck %s

;--- Test.j

.class public Test
.super java/lang/Object
.implements B

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(Ljava/lang/String;)V
.end method

.method public static main([Ljava/lang/String;)V
.limit stack 2
    new Test
    dup
    invokespecial Test/<init>()V
    invokeinterface B/asString()Ljava/lang/String; 1
    ; CHECK: Hello World!
    invokestatic Test/print(Ljava/lang/String;)V
    return
.end method

.method public asString()Ljava/lang/String;
.limit stack 1
    ldc "Hello World!"
    areturn
.end method

;--- B.j

.interface B
.super java/lang/Object
.implements A

;--- A.j

.interface A
.super java/lang/Object

.method public abstract asString()Ljava/lang/String;
.end method
