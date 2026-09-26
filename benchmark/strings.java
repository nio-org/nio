// Mirrors strings.nio in Java. Java strings are immutable too, so every "+"
// allocates a fresh string.
class Main {
    public static void main(String[] args) {
        long n = 100000;
        String target = "abababababababababababababababab";
        long hits = 0;
        long i = 0;

        while (i < n) {
            String s = "";
            long j = 0;
            while (j < 16) {
                s = s + "ab";
                j = j + 1;
            }
            if (s.equals(target)) {
                hits = hits + 1;
            }
            i = i + 1;
        }

        System.out.println(hits);
    }
}
