// Mirrors records.nio in C++: every point and segment is a separate heap
// block. C++ deletes each evicted segment; Nio collects it later.
#include <iostream>

struct point {
    long long x;
    long long y;
};

struct segment {
    point *a;
    point *b;
};

static segment *makeSegment(long long i) {
    segment *s = new segment;
    s->a = new point{i % 1000, (i * 7) % 1000};
    s->b = new point{(i * 13) % 1000, (i * 31) % 1000};
    return s;
}

static void freeSegment(segment *s) {
    delete s->a;
    delete s->b;
    delete s;
}

static long long length2(segment *s) {
    long long dx = s->b->x - s->a->x;
    long long dy = s->b->y - s->a->y;
    return dx * dx + dy * dy;
}

int main() {
    long long n = 2000000;
    segment *recent[4];
    for (int k = 0; k < 4; k++) {
        recent[k] = makeSegment(0);
    }
    long long checksum = 0;
    long long i = 0;

    while (i < n) {
        segment *s = makeSegment(i);
        checksum = (checksum + length2(s)) % 1000000007;
        freeSegment(recent[i % 4]);
        recent[i % 4] = s;
        i = i + 1;
    }

    std::cout << checksum << "\n";
    std::cout << length2(recent[n % 4]) << "\n";
    return 0;
}
