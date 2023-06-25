; RUN: rm -rf %t && split-file %s %t
; RUN: cd %t && jasmin %t/Test.j -d %t && jasmin %t/A.j  && jasmin %t/B.j -d %t
; RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

;--- A.j

.class public A
.super java/lang/Object

.method public <init>()V
    .limit stack 1
    aload_0
    invokenonvirtual java/lang/Object/<init>()V
    return
.end method

.method public a()V
    .limit stack 1
    iconst_5
    invokestatic Test/print(I)V
    return
.end method

;--- B.j

.class public B
.super A

.method public <init>()V
    .limit stack 1
    aload_0
    invokenonvirtual A/<init>()V
    return
.end method

.method public a()V
    .limit stack 1
    iconst_1
    invokestatic Test/print(I)V
    return
.end method

;--- Test.j

.class public Test
.super B

.method public static native print(I)V
.end method

.method public <init>()V
    .limit stack 1
    aload_0
    invokenonvirtual B/<init>()V
    return
.end method

.method public static main([Ljava/lang/String;)V
    .limit stack 2
    new Test
    dup
    invokenonvirtual Test/<init>()V

    ; Should call B/a()V anyways, despite saying it calls A/a() non-virtually.
    invokenonvirtual A/a()V
    return
.end method

; CHECK-NOT: 5
; CHECK: 1
