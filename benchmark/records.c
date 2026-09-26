// Mirrors records.nio in C: every point and segment is a separate heap block.
// C frees each evicted segment; Nio collects it later.
#include <stdio.h>
#include <stdlib.h>

struct point {
    long long x;
    long long y;
};

struct segment {
    struct point *a;
    struct point *b;
};

static struct point *makePoint(long long x, long long y) {
    struct point *p = malloc(sizeof *p);
    p->x = x;
    p->y = y;
    return p;
}

static struct segment *makeSegment(long long i) {
    struct segment *s = malloc(sizeof *s);
    s->a = makePoint(i % 1000, (i * 7) % 1000);
    s->b = makePoint((i * 13) % 1000, (i * 31) % 1000);
    return s;
}

static void freeSegment(struct segment *s) {
    free(s->a);
    free(s->b);
    free(s);
}

static long long length2(struct segment *s) {
    long long dx = s->b->x - s->a->x;
    long long dy = s->b->y - s->a->y;
    return dx * dx + dy * dy;
}

int main(void) {
    long long n = 2000000;
    struct segment *recent[4];
    for (int k = 0; k < 4; k++) {
        recent[k] = makeSegment(0);
    }
    long long checksum = 0;
    long long i = 0;

    while (i < n) {
        struct segment *s = makeSegment(i);
        checksum = (checksum + length2(s)) % 1000000007;
        freeSegment(recent[i % 4]);
        recent[i % 4] = s;
        i = i + 1;
    }

    printf("%lld\n", checksum);
    printf("%lld\n", length2(recent[n % 4]));
    return 0;
}
