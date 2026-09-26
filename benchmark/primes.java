// Counts primes below 2,000,000 by trial division. Mirrors primes.nio in Java.
class Main {
    public static void main(String[] args) {
        long limit = 2000000;
        long count = 1; // 2 is prime
        long i = 3;

        while (i < limit) {
            boolean isPrime = true;
            long j = 3;
            while (j * j <= i && isPrime) {
                if (i % j == 0) {
                    isPrime = false;
                }
                j = j + 2;
            }
            if (isPrime) {
                count = count + 1;
            }
            i = i + 2;
        }

        System.out.println(count);
    }
}
