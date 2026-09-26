// Mirrors files.nio in C, with the same document and a POSIX ERE pattern.
// C frees each intermediate document; Nio collects it later.
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *slurp(const char *path, long long *len) {
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size + 1);
    fread(text, 1, size, f);
    text[size] = '\0';
    fclose(f);
    *len = size;
    return text;
}

static void spit(const char *path, const char *text, long long len) {
    FILE *f = fopen(path, "wb");
    fwrite(text, 1, len, f);
    fclose(f);
}

int main(void) {
    long long n = 1000;
    long long lines = 400;

    char *doc = strdup("");
    long long doclen = 0;
    long long k = 0;
    while (k < lines) {
        char line[64];
        int m = snprintf(line, sizeof line, "field_%lld = value_%lld\n", k, k);
        char *grown = malloc(doclen + m + 1);
        memcpy(grown, doc, doclen);
        memcpy(grown + doclen, line, m + 1);
        free(doc);
        doc = grown;
        doclen = doclen + m;
        k = k + 1;
    }
    char *grown = malloc(doclen + 16);
    memcpy(grown, doc, doclen);
    memcpy(grown + doclen, "counter = 0000\n", 16);
    free(doc);
    doc = grown;
    doclen = doclen + 15;

    char dir[] = "/tmp/nio-bench-files-XXXXXX";
    if (mkdtemp(dir) == NULL) {
        return 1;
    }
    char file[64];
    snprintf(file, sizeof file, "%s/data.txt", dir);
    spit(file, doc, doclen);
    free(doc);

    regex_t re;
    if (regcomp(&re, "counter = [0-9]{4}", REG_EXTENDED) != 0) {
        return 1;
    }
    long long checksum = 0;
    long long i = 0;

    while (i < n) {
        long long len = 0;
        char *text = slurp(file, &len);
        regmatch_t m;
        regexec(&re, text, 1, &m, 0);
        long long at = m.rm_so;
        checksum = checksum + at;

        char digits[5];
        memcpy(digits, text + at + 10, 4);
        digits[4] = '\0';
        long long v = strtoll(digits, NULL, 10) + 1;
        char num[5];
        snprintf(num, sizeof num, "%04lld", v);

        char *edited = malloc(len + 1);
        memcpy(edited, text, at + 10);
        memcpy(edited + at + 10, num, 4);
        memcpy(edited + at + 14, text + at + 14, len - (at + 14) + 1);
        free(text);

        spit(file, edited, len);
        free(edited);
        i = i + 1;
    }

    long long len = 0;
    char *text = slurp(file, &len);
    regmatch_t m;
    regexec(&re, text, 1, &m, 0);
    long long at = m.rm_so;
    char digits[5];
    memcpy(digits, text + at + 10, 4);
    digits[4] = '\0';
    long long counter = strtoll(digits, NULL, 10);
    free(text);
    regfree(&re);

    remove(file);
    rmdir(dir);
    printf("%lld\n", checksum + counter);
    return 0;
}
