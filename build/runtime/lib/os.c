// The `os` standard library module (import 'os'), linked only when a program
// imports it. Signatures must match osCallType in the checker and genOSCall in
// codegen.
//
// nio compiles for the host, so the preprocessor picks the platform and
// architecture literals below. Only getCpus queries the running system, and it
// is the only thing here with per-OS bodies.

#include "runtime.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

// ---- compile-time host identity ----

#if defined(__APPLE__)
#define NIO_PLATFORM "darwin"
#elif defined(_WIN32)
#define NIO_PLATFORM "win32"
#elif defined(__linux__)
#define NIO_PLATFORM "linux"
#elif defined(__OpenBSD__)
#define NIO_PLATFORM "openbsd"
#elif defined(__FreeBSD__)
#define NIO_PLATFORM "freebsd"
#else
#define NIO_PLATFORM "unknown"
#endif

// 64-bit ARM is "arm" and 32-bit ARM "arm32". The width is in the name only
// where it separates two supported targets.
#if defined(__aarch64__) || defined(_M_ARM64)
#define NIO_ARCH "arm"
#elif defined(__arm__) || defined(_M_ARM)
#define NIO_ARCH "arm32"
#elif defined(__x86_64__) || defined(_M_X64)
#define NIO_ARCH "x64"
#elif defined(__i386__) || defined(_M_IX86)
#define NIO_ARCH "x86"
#else
#define NIO_ARCH "unknown"
#endif

Str *rt_os_platform(void) { return rt_str_from_c(NIO_PLATFORM); }

Str *rt_os_arch(void) { return rt_str_from_c(NIO_ARCH); }

// ---- per-platform CPU queries ----
//
// Each platform supplies three functions. cpu_model fills buf, and leaves it
// empty when the system reports no name. cpu_speed answers MHz and cpu_count
// logical cores, each 0 for "not reported". A 0 becomes null in the Cpu record.

#define MODEL_MAX 256

#if defined(_WIN32)

// The registry calls are in advapi32. A driver other than MSVC can need
// -ladvapi32.
#if defined(_MSC_VER)
#pragma comment(lib, "advapi32")
#endif

// Windows has no sysctl-like interface. Processor 0's registry key holds both
// the model and the speed.
#define CPU_KEY "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"

static void cpu_model(char *buf, size_t n) {
    DWORD size = (DWORD)n;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, CPU_KEY, "ProcessorNameString",
                     RRF_RT_REG_SZ, NULL, buf, &size) != ERROR_SUCCESS) {
        buf[0] = 0;
    }
}

static int64_t cpu_speed(void) {
    DWORD mhz = 0, size = sizeof mhz;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, CPU_KEY, "~MHz", RRF_RT_REG_DWORD,
                     NULL, &mhz, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return (int64_t)mhz;
}

static int64_t cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int64_t)si.dwNumberOfProcessors;
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)

static int sysctl_str(const char *name, char *buf, size_t n) {
    size_t len = n;
    if (sysctlbyname(name, buf, &len, NULL, 0) != 0 || len == 0) {
        buf[0] = 0;
        return 0;
    }
    buf[n - 1] = 0; // sysctl NUL-terminates, but do not depend on it
    return 1;
}

// Key widths vary (hw.cpufrequency is 64-bit, hw.cpuspeed 32-bit), so the size
// the kernel reports decides how to read it. The low half of a zeroed 64-bit
// buffer is correct on a little-endian host, which every target is.
static int64_t sysctl_int(const char *name) {
    int64_t wide = 0;
    size_t len = sizeof wide;
    if (sysctlbyname(name, &wide, &len, NULL, 0) != 0) return 0;
    if (len == sizeof(int32_t)) {
        int32_t narrow;
        memcpy(&narrow, &wide, sizeof narrow);
        return narrow;
    }
    return wide;
}

static void cpu_model(char *buf, size_t n) {
#if defined(__APPLE__)
    // brand_string names the chip ("Apple M4"). hw.model names the machine
    // ("Mac16,1"), so it is the fallback.
    if (sysctl_str("machdep.cpu.brand_string", buf, n)) return;
#endif
    sysctl_str("hw.model", buf, n);
}

static int64_t cpu_speed(void) {
#if defined(__APPLE__)
    // Apple Silicon does not have this key, so os.Cpu.speed is null there.
    return sysctl_int("hw.cpufrequency") / 1000000;
#elif defined(__OpenBSD__)
    return sysctl_int("hw.cpuspeed"); // already MHz
#else // FreeBSD
    return sysctl_int("dev.cpu.0.freq"); // already MHz
#endif
}

static int64_t cpu_count(void) {
#if defined(__APPLE__)
    int64_t logical = sysctl_int("hw.logicalcpu");
    if (logical > 0) return logical;
#endif
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int64_t)n : 0;
}

#else // Linux and other systems with /proc and sysconf

static void cpuinfo_value(const char *line, char *buf, size_t n) {
    const char *colon = strchr(line, ':');
    if (!colon) return;
    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    size_t len = strlen(v);
    while (len > 0 && (v[len - 1] == '\n' || v[len - 1] == '\r' ||
                       v[len - 1] == ' ' || v[len - 1] == '\t')) {
        len--;
    }
    if (len > n - 1) len = n - 1;
    memcpy(buf, v, len);
    buf[len] = 0;
}

static int cpuinfo_find(const char *key, char *buf, size_t n) {
    buf[0] = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char line[512];
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0) {
            cpuinfo_value(line, buf, n);
            break;
        }
    }
    fclose(f);
    return buf[0] != 0;
}

static void cpu_model(char *buf, size_t n) {
    // ARM kernels report no "model name". The closest they publish is the
    // board in "Hardware".
    if (cpuinfo_find("model name", buf, n)) return;
    cpuinfo_find("Hardware", buf, n);
}

static int64_t cpu_speed(void) {
    char buf[64];
    // /proc reports the current speed, scaled down on an idle machine, so
    // cpufreq's advertised maximum comes first.
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
    if (f) {
        long khz = 0;
        int got = fscanf(f, "%ld", &khz);
        fclose(f);
        if (got == 1 && khz > 0) return khz / 1000;
    }
    if (cpuinfo_find("cpu MHz", buf, sizeof buf)) {
        double mhz = 0;
        if (sscanf(buf, "%lf", &mhz) == 1 && mhz > 0) return (int64_t)mhz;
    }
    return 0;
}

static int64_t cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int64_t)n : 0;
}

#endif

// ---- os.getCpus() ----

static TypeDesc td_opt_string = {TD_OPTIONAL, &rt_td_string, 0, NULL, NULL, 0};
static TypeDesc td_opt_int = {TD_OPTIONAL, &rt_td_int, 0, NULL, NULL, 0};

// A Cpu record is two slots: [0] model, [1] speed. Both are optionals, so each
// holds null or a pointer to a box.
#define CPU_SLOTS 2

// Stamped into every record this builds (REC_TD, runtime.h). It carries the
// field names because json.c reads a record's shape out of the value. The
// offsets must match what codegen computes for types.CpuT.
static const char *cpu_field_names[CPU_SLOTS] = {"model", "speed"};
static TypeDesc *cpu_field_types[CPU_SLOTS] = {&td_opt_string, &td_opt_int};
static const int64_t cpu_field_offsets[CPU_SLOTS] = {8, 16};
static TypeDesc td_cpu = {TD_RECORD, 0,   CPU_SLOTS, cpu_field_names,
                          cpu_field_types, 0, cpu_field_offsets, 24};

// Answers os.Cpu[]: one record per logical core, all with the same model and
// speed, because no interface above reports per core. A system that reports
// nothing still yields one entry with null fields, so there is always one CPU.
Arr *rt_os_get_cpus(TypeDesc *arr_td) {
    char model[MODEL_MAX];
    cpu_model(model, sizeof model);
    int64_t speed = cpu_speed();
    int64_t n = cpu_count();
    if (n < 1) n = 1;

    // The array is rooted with its real descriptor from the moment it exists,
    // so written entries are traced and the rest trace as null. The other three
    // slots hold one entry's parts while the next allocates.
    TypeDesc *tds[4] = {arr_td, &rt_td_string, &td_opt_string, &td_opt_int};
    int64_t slots[4] = {0, 0, 0, 0};
    GCFrame f = {rt_gc_top, 4, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *a = rt_arr_new(n, 8);
    slots[0] = (int64_t)(intptr_t)a;

    for (int64_t i = 0; i < n; i++) {
        slots[1] = model[0] ? (int64_t)(intptr_t)rt_str_from_c(model) : 0;
        slots[2] = slots[1] ? (int64_t)(intptr_t)rt_box(slots[1], &rt_td_string) : 0;
        slots[3] = speed > 0 ? (int64_t)(intptr_t)rt_box(speed, &rt_td_int) : 0;
        // Nothing allocates until the last store, so the record needs no root.
        void *cpu = rt_rec_new(&td_cpu);
        rt_rec_set(cpu, 0, slots[2], &td_cpu);
        rt_rec_set(cpu, 1, slots[3], &td_cpu);
        rt_arr_units(a)[i] = (int64_t)(intptr_t)cpu;
    }

    rt_gc_top = f.prev;
    return a;
}
