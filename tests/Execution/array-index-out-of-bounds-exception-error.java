// RUN: javac %s -d %t
// RUN: not jllvm %t/Test.class 2>&1 | FileCheck %s

class Test
{
    public static void main(String[] args)
    {
        var arr = new Test[5];
        var test = arr[10];
    }
}

// CHECK: Index 10 out of bounds for length 5
