// RUN: javac %s -d %t
// RUN: not jllvm %t/Test.class 2>&1 | FileCheck %s

class Test
{
    static native void print(int i);

    public static void main(String[] args)
    {
        // CHECK-NOT: {{[0-9]+}}

        // CHECK: RuntimeException
        // CHECK: A message

        // CHECK-NOT: {{[0-9]+}}
        unwindThroughVoid();
        print(5);
    }

    private static void unwindThroughVoid()
    {
        unwindThroughOther();
        print(5);
    }

    private static int unwindThroughOther()
    {
        throw new RuntimeException("A message");
    }
}
