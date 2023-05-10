// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

//--- Test.java

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        Object o = null;

        // CHECK: 0
        print(o instanceof Object ? 1 : 0);

        var array = new Test[1];

        // CHECK: 1
        print(array instanceof Test[] ? 1 : 0);

        // CHECK: 1
        print(array instanceof Object[] ? 1 : 0);

        // CHECK: 1
        print(array instanceof Object ? 1 : 0);

        // CHECK: 0
        print(((Object)array) instanceof Test[][] ? 1 : 0);

        B c = new C();

        // CHECK: 1
        print(c instanceof A ? 1 : 0);

        // CHECK: 1
        print(c instanceof B ? 1 : 0);

        // CHECK: 1
        print(c instanceof C ? 1 : 0);

        // CHECK: 1
        print(c instanceof Object ? 1 : 0);

        // TODO: test array of primitive types once creating those is implemented.
    }
}

//--- A.java

public interface A
{

}

//--- B.java

public interface B extends A
{

}

//--- C.java

public class C implements B
{

}
