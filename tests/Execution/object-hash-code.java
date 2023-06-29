// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        var t = new Test();
        print(t.hashCode());
        // Cause garbage collection (at least in debug builds).
        new Object();
        // Hashcode should remain the same despite object relocation.
        print(t.hashCode());
    }
}

// CHECK: [[HASHCODE:.*]]
// CHECK-NEXT: [[HASHCODE]]
