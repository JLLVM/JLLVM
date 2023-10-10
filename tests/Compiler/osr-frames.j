; RUN: jasmin %s -d %t
; OSR into method entry.
; RUN: jllvm-jvmc --method "test:(I)I" --osr 0 %t/Test.class | FileCheck %s --check-prefixes=CHECK,ENTRY
; OSR into middle of the loop in catch.
; RUN: jllvm-jvmc --method "test:(I)I" --osr 16 %t/Test.class | FileCheck %s --check-prefixes=CHECK,LOOP
; OSR into exception handler.
; RUN: jllvm-jvmc --method "test:(I)I" --osr 29 %t/Test.class | FileCheck %s --check-prefixes=CHECK,EXC

.class public Test
.super java/lang/Object

.field public static foo Ljava/lang/Object;

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(I)V
.end method

; CHECK-LABEL: define i32 @"Test.test:(I)I
; ENTRY-SAME: $0"
; LOOP-SAME: $16"
; EXC-SAME: $29"
; CHECK-SAME: ptr %[[OPERANDS:[[:alnum:]]+]]
; CHECK-SAME: ptr %[[LOCALS:[[:alnum:]]+]]
.method public static test(I)I
; CHECK: %[[OP0:.*]] = alloca ptr
; CHECK: %[[LOCAL0:.*]] = alloca ptr
; CHECK: %[[LOCAL1:.*]] = alloca ptr
    .limit stack 1
    .limit locals 2

; Loops has one operand on the stack and one local.
; Entry has just one local.
; Exc has two locals and no operands on the stack.

; LOOP: %[[GEP:.*]] = getelementptr ptr, ptr %[[OPERANDS]], i32 0
; LOOP: %[[LOAD:.*]] = load i32, ptr %[[GEP]]
; LOOP: store i32 %[[LOAD]], ptr %[[OP0]]
; CHECK: %[[GEP:.*]] = getelementptr ptr, ptr %[[LOCALS]], i32 0
; CHECK: %[[LOAD:.*]] = load i32, ptr %[[GEP]]
; CHECK: store i32 %[[LOAD]], ptr %[[LOCAL0]]
; EXC: %[[GEP:.*]] = getelementptr ptr, ptr %[[LOCALS]], i32 1
; EXC: %[[LOAD:.*]] = load ptr addrspace(1), ptr %[[GEP]]
; EXC: store ptr addrspace(1) %[[LOAD]], ptr %[[LOCAL1]]

; Entry continues to start of bytecode as usual.
; ENTRY: br label %[[ENTRY:[[:alnum:]]+]]

; ENTRY: [[ENTRY]]:
; ENTRY-NEXT: call void @"Static Call to Test.foo:()V"()

; LOOP: br label %[[LOOP_HEADER:[[:alnum:]]+]]

; LOOP-NOT: call void @"Static Call to Test.foo:()V"

; EXC: br label %[[THROW_CODE:[[:alnum:]]+]]

    invokestatic Test/foo()V
L3:
    invokestatic Test/condition()Z
    ifeq L34
L9:
    iinc 0 1
L15:
    iload_0
; 16:

; LOOP: [[LOOP_HEADER]]:
; LOOP-NEXT: %[[ARG:.*]] = load i32, ptr %[[OP0]]
; LOOP-NEXT: call void @"Static Call to Test.bar:(I)V"(
; LOOP-SAME: i32{{.*}}%[[ARG]]
; LOOP-SAME: )
; LOOP: br i1 %{{.*}}, label %{{.*}}, label %[[EXC_HANDLER:[[:alnum:]]+]]

; Check that despite OSRing into the middle of a 'try', that the exception handler after the bar call still leads to
; calling the finally block.

; LOOP: [[EXC_HANDLER]]:
; LOOP-NEXT: %[[PHI:.*]] = phi
; LOOP-NEXT: store ptr addrspace(1) null, ptr @activeException
; LOOP-NEXT: store ptr addrspace(1) %[[PHI]], ptr %[[OP0]]
; LOOP-NEXT: br label %[[L25CODE:[[:alnum:]]+]]

; Check that the basic block was split correctly and that the iinc and iload_0 still lead to the OSR entry.

; LOOP: %[[ADD:.*]] = add i32 %{{.*}}, 1
; LOOP-NEXT: store i32 %[[ADD]], ptr %[[LOCAL0]]
; LOOP-NEXT: %[[LOCAL0_LOAD:.*]] = load i32, ptr %[[LOCAL0]]
; LOOP-NEXT: store i32 %[[LOCAL0_LOAD]], ptr %[[OP0]]
; LOOP-NEXT: br label %[[LOOP_HEADER]]

; LOOP: {{^}}[[L25CODE]]:
; LOOP-NEXT: %[[OP0_LOAD:.*]] = load ptr addrspace(1), ptr %[[OP0]]
; LOOP-NEXT: store ptr addrspace(1) %[[OP0_LOAD]], ptr %[[LOCAL1]]
; LOOP-NEXT: call void @"Static Call to Test.foobar:()V"()

    invokestatic Test/bar(I)V
L19:
    invokestatic Test/foobar()V
    goto L31
L25:
    astore_1
    invokestatic Test/foobar()V
; 29:

; EXC: [[THROW_CODE]]:
; EXC-NEXT: %[[LOAD_LOCAL1:.*]] = load ptr addrspace(1), ptr %[[LOCAL1]]
; EXC-NEXT: store ptr addrspace(1) %[[LOAD_LOCAL1]], ptr %[[OP0]]
; EXC-NEXT: %[[LOAD_OP0:.*]] = load ptr addrspace(1), ptr %[[OP0]]

; EXC: store ptr addrspace(1) %[[LOAD_OP0]], ptr @activeException
    aload_1
    athrow
L31:
    goto L3
L34:
    iload_0
    ireturn

.catch all from L15 to L19 using L25
.end method
