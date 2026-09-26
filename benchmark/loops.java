// 100,000,000 iterations of a nested loop. Mirrors loops.nio in Java.
class Main {
    public static void main(String[] args) {
        long u = 10;
        int r = 5000;
        long[] a = new long[10000];
        int i = 0;

        while (i < 10000) {
            long j = 0;
            while (j < 10000) {
                a[i] = a[i] + j % u;
                j = j + 1;
            }
            a[i] = a[i] + r;
            i = i + 1;
        }

        System.out.println(a[r]);
    }
}
