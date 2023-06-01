// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: not jllvm -Xenable-test-utils %t/Test.class 2>&1 | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(String i);

    public static void main(String[] args)
    {
        Object o = new Test();
        var t = (Test)o;
        var z = (Other)o;
    }
}

// CHECK: class Test cannot be cast to class Other

//--- Other.java

public class Other
{

}
