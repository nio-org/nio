// Counts primes below 2,000,000 by trial division. Mirrors primes.nio in C++.
#include <iostream>

int main() {
    long long limit = 2000000;
    long long count = 1; // 2 is prime
    long long i = 3;

    while (i < limit) {
        bool isPrime = true;
        long long j = 3;
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

    std::cout << count << "\n";
    return 0;
}
