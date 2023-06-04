; RUN: jasmin %s -d %t
; RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s
.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(I)V
.end method

.method public static native print(J)V
.end method

.method public static dup()V
    .limit stack 2
    iconst_0
    dup

    ; CHECK: 0
    invokestatic Test/print(I)V
    ; CHECK: 0
    invokestatic Test/print(I)V
    return
.end method

.method public static dup_x1()V
    .limit stack 3
    iconst_1
    iconst_2
    dup_x1

    ; CHECK: 2
    invokestatic Test/print(I)V
    ; CHECK: 1
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V
    return
.end method

.method public static dup_x2()V
    .limit stack 4
    iconst_3
    iconst_4
    iconst_5
    ; Form 1:
    dup_x2

    ; CHECK: 5
    invokestatic Test/print(I)V
    ; CHECK: 4
    invokestatic Test/print(I)V
    ; CHECK: 3
    invokestatic Test/print(I)V
    ; CHECK: 5
    invokestatic Test/print(I)V

    lconst_0
    iconst_1
    ; Form 2:
    dup_x2

    ; CHECK: 1
    invokestatic Test/print(I)V
    ; CHECK: 0
    invokestatic Test/print(J)V
    ; CHECK: 1
    invokestatic Test/print(I)V
    return
.end method

.method public static dup2()V
    .limit stack 4
    iconst_2
    iconst_3
    ; Form 1:
    dup2

    ; CHECK: 3
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V
    ; CHECK: 3
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V

    lconst_1
    ; Form 2:
    dup2

    ; CHECK: 1
    invokestatic Test/print(J)V
    ; CHECK: 1
    invokestatic Test/print(J)V
    return
.end method

.method public static dup2_x1()V
    .limit stack 5
    iconst_3
    iconst_2
    iconst_1
    ; Form 1:
    dup2_x1

    ; CHECK: 1
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V
    ; CHECK: 3
    invokestatic Test/print(I)V
    ; CHECK: 1
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V

    iconst_2
    lconst_1
    ; Form 1:
    dup2_x1

    ; CHECK: 1
    invokestatic Test/print(J)V
    ; CHECK: 2
    invokestatic Test/print(I)V
    ; CHECK: 1
    invokestatic Test/print(J)V
    return
.end method

.method public static dup2_x2()V
    .limit stack 6
    iconst_4
    iconst_3
    iconst_2
    iconst_1
    ; Form 1:
    dup2_x2

    ; CHECK: 1
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V
    ; CHECK: 3
    invokestatic Test/print(I)V
    ; CHECK: 4
    invokestatic Test/print(I)V
    ; CHECK: 1
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V

    iconst_3
    iconst_2
    lconst_1
    ; Form 2:
    dup2_x2

    ; CHECK: 1
    invokestatic Test/print(J)V
    ; CHECK: 2
    invokestatic Test/print(I)V
    ; CHECK: 3
    invokestatic Test/print(I)V
    ; CHECK: 1
    invokestatic Test/print(J)V

    ldc2_w  3
    iconst_2
    iconst_1
    ; Form 3:
    dup2_x2

    ; CHECK: 1
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V
    ; CHECK: 3
    invokestatic Test/print(J)V
    ; CHECK: 1
    invokestatic Test/print(I)V
    ; CHECK: 2
    invokestatic Test/print(I)V

    ldc2_w  2
    lconst_1
    ; Form 3:
    dup2_x2

    ; CHECK: 1
    invokestatic Test/print(J)V
    ; CHECK: 2
    invokestatic Test/print(J)V
    ; CHECK: 1
    invokestatic Test/print(J)V
    return
.end method

.method public static nop()V
    .limit stack 1
    iconst_0
    nop

    ; CHECK: 0
    invokestatic Test/print(I)V
    return
.end method

.method public static pop()V
    .limit stack 2
    iconst_1
    iconst_2
    pop

    ; CHECK-NOT: 2
    ; CHECK: 1
    invokestatic Test/print(I)V
    return
.end method

.method public static pop2()V
    .limit stack 3
    iconst_3
    iconst_4
    iconst_5
    ; Form 1:
    pop2

    ; CHECK-NOT: 5
    ; CHECK-NOT: 4
    ; CHECK: 3
    invokestatic Test/print(I)V

    iconst_0
    lconst_1
    ; Form 2:
    pop2

    ; CHECK-NOT: 1
    ; CHECK: 0
    invokestatic Test/print(I)V
    return
.end method

.method public static swap()V
    .limit stack 2
    iconst_2
    iconst_3
    swap

    ; CHECK-NOT: 3
    ; CHECK: 2
    invokestatic Test/print(I)V

    ; CHECK-NOT: 2
    ; CHECK: 3
    invokestatic Test/print(I)V
    return
.end method

.method public static main([Ljava/lang/String;)V
    ; dup
    invokestatic Test/dup()V

    ; dup_x1
    invokestatic Test/dup_x1()V

    ; dup_x2
    invokestatic Test/dup_x2()V

    ; dup2
    invokestatic Test/dup2()V

    ; dup2_x1
    invokestatic Test/dup2_x1()V

    ; dup2_x2
    invokestatic Test/dup2_x2()V

    ; nop
    invokestatic Test/nop()V

    ; pop
    invokestatic Test/pop()V

    ; pop2
    invokestatic Test/pop2()V

    ; swap
    invokestatic Test/swap()V

    return
.end method
