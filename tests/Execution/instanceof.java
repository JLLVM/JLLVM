// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

//--- Test.java

class Test
{
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        Object o = null;

        // CHECK: 0
        print(o instanceof Object);

        var pArray = new int[1];

        // CHECK: 1
        print(pArray instanceof int[]);

        // CHECK: 1
        print(pArray instanceof Object);

        // CHECK: 1
        print(pArray instanceof Cloneable);

        // CHECK: 0
        print(((Object)pArray) instanceof int[][]);

        var aArray = new Test[1];

        // CHECK: 1
        print(aArray instanceof Test[]);

        // CHECK: 1
        print(aArray instanceof Object[]);

        // CHECK: 1
        print(aArray instanceof Object);

        // CHECK: 0
        print(((Object)aArray) instanceof Test[][]);

        B c = new C();

        // CHECK: 1
        print(c instanceof A);

        // CHECK: 1
        print(c instanceof B);

        // CHECK: 1
        print(c instanceof C);

        // CHECK: 1
        print(c instanceof Object);

        var mpArray = new int[1][1];

        // CHECK: 1
        print(mpArray instanceof Object[]);
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
