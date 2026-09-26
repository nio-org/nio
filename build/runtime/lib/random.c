// The `random` standard library module. The build links it only for a program
// that imports 'random'. The signatures must match randomCallType in the
// checker and genRandomCall in codegen.
//
// The generator is xoshiro256++ (Blackman and Vigna), with one 256-bit state
// for the program. The first use seeds it from the operating system, so two
// runs differ. rt_random_seed replaces the state, so a run can be repeated.
// Do not use it for secrets: the full stream follows from the seed. For
// unpredictable bytes, use crypto.randomBytes, which reads the kernel
// generator.

#include "runtime.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <sys/random.h>
#else
#include <errno.h>
#include <sys/random.h>
#endif

static uint64_t rng[4];
static int rng_seeded = 0;

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

// SplitMix64 spreads a 64-bit seed over the 256-bit state, as the xoshiro
// authors recommend. A state of small numbers gives a poor start.
static void seed_state(uint64_t s) {
    for (int i = 0; i < 4; i++) {
        s += 0x9E3779B97F4A7C15ULL;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        rng[i] = z ^ (z >> 31);
    }
    rng_seeded = 1;
}

static void os_seed(void) {
    uint64_t s = 0;
#if defined(_WIN32)
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)&s, (ULONG)sizeof s,
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        rt_panic("random: the system random generator failed");
    }
#elif defined(__APPLE__)
    if (getentropy(&s, sizeof s) != 0) {
        rt_panic("random: the system random generator failed");
    }
#else
    uint8_t *p = (uint8_t *)&s;
    size_t n = sizeof s;
    while (n > 0) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            rt_panic("random: the system random generator failed");
        }
        p += (size_t)got;
        n -= (size_t)got;
    }
#endif
    seed_state(s);
}

static uint64_t next(void) {
    if (!rng_seeded) os_seed();
    uint64_t result = rotl(rng[0] + rng[3], 23) + rng[0];
    uint64_t t = rng[1] << 17;
    rng[2] ^= rng[0];
    rng[3] ^= rng[1];
    rng[1] ^= rng[2];
    rng[0] ^= rng[3];
    rng[2] ^= t;
    rng[3] = rotl(rng[3], 45);
    return result;
}

void rt_random_seed(int64_t s) { seed_state((uint64_t)s); }

// Returns a value in [lo, hi], each value equally likely. Uses Lemire's
// multiply-and-reject method, which removes the bias of a modulo.
int64_t rt_random_int(int64_t lo, int64_t hi) {
    if (lo > hi) rt_panic("random.int: the low bound is above the high bound");
    uint64_t range = (uint64_t)hi - (uint64_t)lo + 1;
    if (range == 0) return (int64_t)next();    // the whole range: every value
    uint64_t x = next();
    __uint128_t m = (__uint128_t)x * range;
    uint64_t l = (uint64_t)m;
    if (l < range) {
        uint64_t t = (0 - range) % range;
        while (l < t) {
            x = next();
            m = (__uint128_t)x * range;
            l = (uint64_t)m;
        }
    }
    return (int64_t)((uint64_t)lo + (uint64_t)(m >> 64));
}

// Returns a value in [0, 1) with 53 random bits, the precision of a double.
double rt_random_float(void) {
    return (double)(next() >> 11) * (1.0 / 9007199254740992.0);
}

// Fisher-Yates shuffle of the element block, at the element width (§5.7).
// Nothing here allocates, so nothing needs a root.
void rt_random_shuffle(Arr *a) {
    uint8_t *d = (uint8_t *)a->data;
    size_t w = (size_t)a->width;
    uint8_t tmp[8];
    for (int64_t i = a->len - 1; i > 0; i--) {
        int64_t j = rt_random_int(0, i);
        if (j == i) continue;
        memcpy(tmp, d + (size_t)i * w, w);
        memcpy(d + (size_t)i * w, d + (size_t)j * w, w);
        memcpy(d + (size_t)j * w, tmp, w);
    }
}

// Returns one element as its 8-byte unit. arr_td describes the array, so the
// element is read at its own width. An empty array stops the program.
int64_t rt_random_pick(Arr *a, TypeDesc *arr_td) {
    if (a->len == 0) rt_panic("random.pick: the array is empty");
    return rt_arr_get(a, rt_random_int(0, a->len - 1), arr_td->elem);
}
