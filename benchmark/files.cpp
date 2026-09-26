// files.nio in C++. The pattern uses std::regex.
// Assignment frees the old document immediately. Nio collects it later.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unistd.h>

static std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

static void spit(const std::string &path, const std::string &text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

int main() {
    long long n = 1000;
    long long lines = 400;

    std::string doc = "";
    long long k = 0;
    while (k < lines) {
        doc = doc + "field_" + std::to_string(k) + " = value_" + std::to_string(k) + "\n";
        k = k + 1;
    }
    doc = doc + "counter = 0000\n";

    char dir[] = "/tmp/nio-bench-files-XXXXXX";
    if (mkdtemp(dir) == nullptr) {
        return 1;
    }
    std::string file = std::string(dir) + "/data.txt";
    spit(file, doc);

    std::regex re("counter = [0-9]{4}");
    long long checksum = 0;
    long long i = 0;

    while (i < n) {
        std::string text = slurp(file);
        std::smatch m;
        std::regex_search(text, m, re);
        long long at = m.position(0);
        checksum = checksum + at;

        long long v = std::stoll(text.substr(at + 10, 4)) + 1;
        char num[5];
        snprintf(num, sizeof num, "%04lld", v);
        std::string edited = text.substr(0, at + 10) + num + text.substr(at + 14);

        spit(file, edited);
        i = i + 1;
    }

    std::string text = slurp(file);
    std::smatch m;
    std::regex_search(text, m, re);
    long long at = m.position(0);
    long long counter = std::stoll(text.substr(at + 10, 4));

    remove(file.c_str());
    rmdir(dir);
    std::cout << checksum + counter << "\n";
    return 0;
}
