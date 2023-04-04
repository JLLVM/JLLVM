// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test {
    public static native void print(int i);

    static void square(int num) {
        print(num < 6 ? 5 : -3);
    }

    public static void main(String[] args) {
        //CHECK: 5
        square(5);
        //CHECK: 5
        square(-1);
        //CHECK: -3
        square(6);
    }
}