// Mirrors strings.nio in C: every append allocates a fresh buffer and copies.
// C frees each intermediate; Nio collects it later.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *concat(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *r = malloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}

int main(void) {
    long long n = 100000;
    const char *target = "abababababababababababababababab";
    long long hits = 0;
    long long i = 0;

    while (i < n) {
        char *s = strdup("");
        long long j = 0;
        while (j < 16) {
            char *t = concat(s, "ab");
            free(s);
            s = t;
            j = j + 1;
        }
        if (strcmp(s, target) == 0) {
            hits = hits + 1;
        }
        free(s);
        i = i + 1;
    }

    printf("%lld\n", hits);
    return 0;
}
