// RUN: javac -encoding utf-8 %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(boolean i);
    public static native void print(byte i);
    public static native void print(char i);
    public static native void print(int i);
    public static native void print(short i);

    public static void main(String[] args)
    {
        byte[] bytes = {Byte.MIN_VALUE, -4, 4, Byte.MAX_VALUE};
        boolean[] booleans = {true, false, false, true};
        char[] chars = {'a', 'b', 'c', '☕'};
        short[] shorts = {Short.MIN_VALUE, -4, 4, Short.MAX_VALUE};

        // Testing byte
        // CHECK: -128
        print(bytes[0]);
        // CHECK: -4
        print(bytes[1]);
        // CHECK: 4
        print(bytes[2]);
        // CHECK: 127
        print(bytes[3]);
        // CHECK: 4
        print(bytes.length);

        // Testing boolean
        // CHECK: 1
        print(booleans[0]);
        // CHECK: 0
        print(booleans[1]);
        // CHECK: 0
        print(booleans[2]);
        // CHECK: 1
        print(booleans[3]);
        // CHECK: 4
        print(booleans.length);

        // Testing char
        // CHECK: 97
        print(chars[0]);
        // CHECK: 98
        print(chars[1]);
        // CHECK: 99
        print(chars[2]);
        // CHECK: 9749
        print(chars[3]);
        // CHECK: 4
        print(chars.length);

        // Testing short
        // CHECK: -32768
        print(shorts[0]);
        // CHECK: -4
        print(shorts[1]);
        // CHECK: 4
        print(shorts[2]);
        // CHECK: 32767
        print(shorts[3]);
        // CHECK: 4
        print(shorts.length);
    }
}
