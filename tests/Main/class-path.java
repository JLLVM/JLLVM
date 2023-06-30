// RUN: rm -rf %t && split-file %s %t
// RUN: javac %t/SomeOtherDir/Other.java -d %t/SomeOtherDir
// RUN: javac %t/SomeSecondDir/Otherwise.java -d %t/SomeSecondDir
// RUN: javac -cp '%t/SomeOtherDir:%t/SomeSecondDir' %t/Test.java -d %t

// RUN: jllvm -Xno-system-init -cp '%t/SomeOtherDir;%t/SomeSecondDir' -Xenable-test-utils %t/Test.class | FileCheck %s
// RUN: jllvm -Xno-system-init --class-path '%t/SomeOtherDir;%t/SomeSecondDir' -Xenable-test-utils %t/Test.class | FileCheck %s
// RUN: jllvm -Xno-system-init -classpath '%t/SomeOtherDir;%t/SomeSecondDir' -Xenable-test-utils %t/Test.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        // CHECK: 5
        // CHECK: 3
        print(Other.i);
        print(Otherwise.i);
    }
}

//--- SomeOtherDir/Other.java

public class Other
{
    public static int i = 5;
}

//--- SomeSecondDir/Otherwise.java

public class Otherwise
{
    public static int i = 3;
}
