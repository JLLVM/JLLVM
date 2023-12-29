; RUN: rm -rf %t && split-file %s %t
; RUN: cd %t && jasmin %t/Test.j -d %t && jasmin %t/Other.j -d %t
; RUN: not jllvm -Xint %t/Test.class 2>&1 | FileCheck %s

;--- Test.j

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static main([Ljava/lang/String;)V
.limit locals 1
.limit stack 1
    new Test
    ; CHECK: class Test cannot be cast to class Other
    checkcast Other
    return
.end method

;--- Other.j

.class public Other
.super java/lang/Object
