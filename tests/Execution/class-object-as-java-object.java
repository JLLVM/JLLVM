// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(boolean i);

    public static void main(String[] args)
    {
        var clazz = Object.class.getClass();
        // CHECK: 1
        print(clazz == clazz.getClass());

        var array = Object[].class;
        // CHECK: 1
        print(array.isArray());
        // CHECK: 1
        print(array.getComponentType() == Object.class);

        var o = new Object();
        // CHECK: 1
        print(o.getClass() == Object.class);
    }
}
