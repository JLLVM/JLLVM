// RUN: rm -rf %t
// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class

import java.util.*;

class Test
{
    interface I {}

    interface J extends I {}

    interface H extends J {}

    class C implements I {}

    class D implements H {}

    public static native void print(boolean b);
    public static native void print(String s);

    public static void main(String[] args)
    {
        // CHECK: 0
        print(Object.class.isInstance(null));
        // CHECK: 1
        print(Object.class.isInstance(new Test()));
        // CHECK: 1
        print(Object.class.isInstance(new int[0]));
        // CHECK: 1
        print(Object[].class.isInstance(new int[0][0]));
        // CHECK: 1
        print(Cloneable.class.isInstance(new int[0]));
        // CHECK: 0
        print(int.class.isInstance(new Test()));

        // CHECK: 1
        print(Object.class.isAssignableFrom(Test.class));
        // CHECK: 1
        print(Object.class.isAssignableFrom(int[].class));
        // CHECK: 1
        print(Object[].class.isAssignableFrom(int[][].class));
        // CHECK: 1
        print(Cloneable.class.isAssignableFrom(int[].class));
        // CHECK: 0
        print(long.class.isAssignableFrom(int.class));

        // CHECK: boolean
        print(boolean.class.getName());
        // CHECK: [I
        print(int[].class.getName());
        // CHECK: java.lang.String
        print(String.class.getName());

        // CHECK: java.lang.Object
        print(Test.class.getSuperclass().getName());
        // CHECK: 1
        print(Object.class.getSuperclass() == null);
        // CHECK: 1
        print(Runnable.class.getSuperclass() == null);
        // CHECK: 1
        print(int.class.getSuperclass() == null);

        // CHECK: []
        print(Arrays.toString(D.class.getInterfaces()));
        // CHECK: [interface Test$H]
        print(Arrays.toString(Test.class.getInterfaces()));
        // CHECK: [interface Test$J]
        print(Arrays.toString(H.class.getInterfaces()));
        // CHECK: []
        print(Arrays.toString(int.class.getInterfaces()));
        // CHECK: [interface java.lang.Cloneable, interface java.io.Serializable]
        print(Arrays.toString(char[].class.getInterfaces()));
    }
}
