; RUN: rm -rf %t && split-file %s %t
; RUN: cd %t && jasmin %t/Test.j -d %t && jasmin %t/Other.j -d %t
; RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

;--- Test.j

.class public Test
.super java/lang/Object
.implements Other

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
    invokeinterface Other/toString()Ljava/lang/String; 1
    ; CHECK: Hello World!
    invokestatic Test/print(Ljava/lang/String;)V
    return
.end method

.method public toString()Ljava/lang/String;
.limit stack 1
    ldc "Hello World!"
    areturn
.end method

;--- Other.j

.interface Other
.super java/lang/Object
