// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        var clazz = Object.class.getClass();
        // CHECK: 1
        print(clazz == clazz.getClass() ? 1 : 0);

        var array = Object[].class;
        // CHECK: 1
        print(array.isArray() ? 1 : 0);
        // CHECK: 1
        print(array.getComponentType() == Object.class ? 1 : 0);

        var o = new Object();
        // CHECK: 1
        print(o.getClass() == Object.class ? 1 : 0);
    }
}
