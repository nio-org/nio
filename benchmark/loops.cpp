// 100,000,000 iterations of a nested loop. Mirrors loops.nio in C++.
#include <iostream>

int main() {
    long long u = 10;
    long long r = 5000;
    long long a[10000] = {0};
    long long i = 0;

    while (i < 10000) {
        long long j = 0;
        while (j < 10000) {
            a[i] = a[i] + j % u;
            j = j + 1;
        }
        a[i] = a[i] + r;
        i = i + 1;
    }

    std::cout << a[r] << "\n";
    return 0;
}
