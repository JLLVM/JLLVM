; RUN: jasmin %s -d %t
; RUN: jllvm -Xenable-test-utils %t/Test.class

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(D)V
.end method

.method public static native print(F)V
.end method

.method public static native print(I)V
.end method

.method public static native print(J)V
.end method

.method public static native print(Ljava/lang/String;)V
.end method

.method public static print(LTest;)V
    ldc "Test"
    invokestatic  Test/print(Ljava/lang/String;)V
    return
.end method

.method public static goto_w(I)V
    .limit stack 2
    iload_0
    iconst_1
    if_icmpne Else
    iconst_0
    invokestatic  Test/print(I)V
    goto_w End
Else:
    iconst_1
    invokestatic  Test/print(I)V
End:
    return
.end method

.method public static wide_a(LTest;)V
    .limit stack 2
    .limit locals 300
    aload_0
    astore_w 260
    aload_w 260
    invokestatic  Test/print(LTest;)V
    return
.end method

.method public static wide_d(D)V
    .limit stack 2
    .limit locals 300
    dload_0
    dstore_w 260
    dload_w 260
    invokestatic  Test/print(D)V
    return
.end method

.method public static wide_f(F)V
    .limit stack 2
    .limit locals 300
    fload_0
    fstore_w 260
    fload_w 260
    invokestatic  Test/print(F)V
    return
.end method

.method public static wide_i(I)V
    .limit locals 300
    iload_0
    istore_w 260
    iload_w 260
    invokestatic  Test/print(I)V
    return
.end method

.method public static wide_l(J)V
    .limit stack 2
    .limit locals 300
    lload_0
    lstore_w 260
    lload_w 260
    invokestatic  Test/print(J)V
    return
.end method

.method public static wide_iinc(I)V
    .limit stack 2
    .limit locals 300
    iinc_w 0 260
    iload_0
    invokestatic  Test/print(I)V
    return
.end method

.method public static main([Ljava/lang/String;)V
.limit stack 4
    iconst_0
    ; CHECK: 1
    invokestatic  Test/goto_w(I)V

    iconst_1
    ; CHECK: 0
    invokestatic  Test/goto_w(I)V

    new Test
    ; CHECK: Test
    invokestatic  Test/wide_a(LTest;)V

    dconst_0
    ; CHECK: 0
    invokestatic  Test/wide_d(D)V

    fconst_1
    ; CHECK: 1
    invokestatic  Test/wide_f(F)V

    iconst_2
    ; CHECK: 2
    invokestatic  Test/wide_i(I)V

    ldc2_w  3
    ; CHECK: 3
    invokestatic  Test/wide_l(J)V

    iconst_4
    ; CHECK: 264
    invokestatic  Test/wide_iinc(I)V

    return
.end method
