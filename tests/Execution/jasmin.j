; RUN: jasmin %s -d %t
; RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s
.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokenonvirtual java/lang/Object/<init>()V
    return
.end method

.method public static native print(Ljava/lang/String;)V
.end method

.method public static main([Ljava/lang/String;)V
    ldc "Hello Jasmin!"

    ; CHECK: Hello Jasmin!
    invokestatic Test/print(Ljava/lang/String;)V

    return
.end method
