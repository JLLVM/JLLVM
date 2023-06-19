// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class

class Test
{
    public static native void print(long l);
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        // Can hardly test precision, time taken or more. Only really testing its a positive integer and that it is
        // monotonic.
        var first = System.nanoTime();
        var second = System.nanoTime();

        // CHECK-NOT: -
        // CHECK: {{[0-9]+}}
        print(first);
        // CHECK-NOT: -
        // CHECK: {{[0-9]+}}
        print(second);
        // CHECK: 1
        print(second >= first);
    }
}
