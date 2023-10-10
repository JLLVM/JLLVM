// RUN: javac %s -d %t
// RUN: not jllvm-jvmc --method "foo:()V" --method "foo:()V" 2>&1 | FileCheck %s --check-prefix=TWO_METHOD
// RUN: not jllvm-jvmc --method "foo()V" 2>&1 | FileCheck %s --check-prefix=INVAL_METHOD
// RUN: not jllvm-jvmc --method "foo:V" 2>&1 | FileCheck %s --check-prefix=INVAL_DESC
// RUN: not jllvm-jvmc --method "foo:()V" %t/Test.class %t/Test.class 2>&1 | FileCheck %s --check-prefix=TWO_INPUT
// RUN: not jllvm-jvmc --method "foo:()V" %t/Bar.class 2>&1 | FileCheck %s --check-prefix=INVAL_INPUT
// RUN: not jllvm-jvmc --method "bar:()V" %t/Test.class 2>&1 | FileCheck %s --check-prefix=NO_METHOD
// RUN: not jllvm-jvmc --method "foo:()V" --osr t %t/Test.class 2>&1 | FileCheck %s --check-prefix=OSR_NUMBER

// TWO_METHOD: expected exactly one occurrence of '--method'
// INVAL_METHOD: expected method in format '<name>:<descriptor>'
// INVAL_DESC: invalid method descriptor 'V'
// TWO_INPUT: expected exactly one input class file
// INVAL_INPUT: failed to open
// INVAL_INPUT-SAME: {{(/|\\\\)}}Bar.class{{[[:space:]]}}
// NO_METHOD: failed to find method 'bar:()V' in 'Test'
// OSR_NUMBER: invalid integer 't' as argument to '--osr'

class Test
{
    void foo()
    {

    }
}
