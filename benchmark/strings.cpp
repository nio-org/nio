// Mirrors strings.nio in C++. Assignment frees the old buffer at once, and
// std::string keeps short intermediates off the heap.
#include <iostream>
#include <string>

int main() {
    long long n = 100000;
    std::string target = "abababababababababababababababab";
    long long hits = 0;
    long long i = 0;

    while (i < n) {
        std::string s = "";
        long long j = 0;
        while (j < 16) {
            s = s + "ab";
            j = j + 1;
        }
        if (s == target) {
            hits = hits + 1;
        }
        i = i + 1;
    }

    std::cout << hits << "\n";
    return 0;
}
