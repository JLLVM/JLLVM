// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(byte i);
    public static native void print(short i);
    public static native void print(char i);

    public static void printValue(byte value)
    {
        print(value);
    }

    public static void printValue(short value)
    {
        print(value);
    }

    public static void printValue(char value)
    {
        print(value);
    }

    public static void main(String[] args)
    {
        // CHECK: 3
        printValue((byte)3);
        // CHECK: 4
        printValue((short)4);
        // CHECK: 5
        printValue((char)5);
    }
}
