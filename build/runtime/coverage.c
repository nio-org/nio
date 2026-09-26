// Statement coverage: this turns the counters generated code increased into an
// LCOV file when the program exits. The build links this file only into a
// binary made with nio --coverage, so an ordinary program does not carry it.
//
// The report is by line, but the counting is by basic block (see the coverage
// code in codegen). Every line of a block reports the counter of that block.
//
// The write happens through atexit, because many runs exit before main ends:
// test.run() calls process.exit(1) on a failure, and a panic also exits.

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The default output path. NIO_COVERAGE_FILE replaces it, so a set of CI jobs
// can write one file each and join them after.
#define COV_DEFAULT_FILE "coverage.lcov"

static const int64_t *cov_counts;
static const CovLine *cov_lines;
static int64_t cov_nlines;

// The report for one line. LCOV needs the table grouped by file and sorted by
// increasing line.
typedef struct {
    const char *file;
    int64_t line;
    int64_t count;
} row;

static int row_cmp(const void *pa, const void *pb) {
    const row *a = (const row *)pa, *b = (const row *)pb;
    int c = strcmp(a->file, b->file);
    if (c != 0) {
        return c;
    }
    return a->line < b->line ? -1 : a->line > b->line ? 1 : 0;
}

// Writes the LCOV file, and a summary of one line to stderr.
//
// Two rows for one line add together and never replace each other. A line can
// carry more than one counter: the post clause of a for loop is emitted at two
// program points, and two types that extend the same base each compile their
// own copy of its methods (§2.4) over the lines of the base.
static void cov_write(void) {
    const char *path = getenv("NIO_COVERAGE_FILE");
    if (path == NULL || *path == '\0') {
        path = COV_DEFAULT_FILE;
    }

    if (cov_nlines <= 0) {
        // Write the file even with nothing to instrument, so a CI step that
        // always uploads it does not fail.
        FILE *empty = fopen(path, "wb");
        if (empty != NULL) {
            fclose(empty);
        }
        fprintf(stderr, "coverage: no instrumented lines\n");
        return;
    }

    row *rows = (row *)malloc(sizeof(row) * (size_t)cov_nlines);
    if (rows == NULL) {
        fprintf(stderr, "coverage: out of memory\n");
        return;
    }
    for (int64_t i = 0; i < cov_nlines; i++) {
        rows[i].file = cov_lines[i].file;
        rows[i].line = cov_lines[i].line;
        rows[i].count = cov_counts[cov_lines[i].block];
    }
    qsort(rows, (size_t)cov_nlines, sizeof(row), row_cmp);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "coverage: cannot write %s\n", path);
        free(rows);
        return;
    }

    int64_t total_found = 0, total_hit = 0;
    int64_t i = 0;
    while (i < cov_nlines) {
        const char *file = rows[i].file;
        int64_t start = i;
        while (i < cov_nlines && strcmp(rows[i].file, file) == 0) {
            i++;
        }
        fprintf(f, "TN:\nSF:%s\n", file);
        int64_t found = 0, hit = 0;
        for (int64_t j = start; j < i;) {
            int64_t line = rows[j].line, count = 0;
            while (j < i && rows[j].line == line) {
                count += rows[j].count;
                j++;
            }
            fprintf(f, "DA:%lld,%lld\n", (long long)line, (long long)count);
            found++;
            if (count > 0) {
                hit++;
            }
        }
        fprintf(f, "LF:%lld\nLH:%lld\nend_of_record\n", (long long)found, (long long)hit);
        total_found += found;
        total_hit += hit;
    }

    fclose(f);
    free(rows);

    double pct = total_found > 0 ? (double)total_hit * 100.0 / (double)total_found : 0.0;
    fprintf(stderr, "coverage: %.1f%% of lines (%lld/%lld) written to %s\n",
            pct, (long long)total_hit, (long long)total_found, path);
}

void rt_cov_init(const int64_t *counts, const CovLine *lines, int64_t nlines) {
    cov_counts = counts;
    cov_lines = lines;
    cov_nlines = nlines;
    atexit(cov_write);
}
