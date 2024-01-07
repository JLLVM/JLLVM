// RUN: not --crash jllvm -Xback-edge-threshold=0x1 Test.class 2>&1 | FileCheck %s
// RUN: not --crash jllvm -Xback-edge-threshold=aba Test.class 2>&1 | FileCheck %s
// RUN: not --crash jllvm -Xback-edge-threshold=10a Test.class 2>&1 | FileCheck %s

// CHECK: Invalid command line argument '-Xback-edge-threshold=
