// Mandelbrot escape-time over a 1000x1000 grid. Mirrors mandelbrot.nio:
// float accumulators for the coordinates, and no break.
#include <iostream>

int main() {
    long long width = 1000;
    long long height = 1000;
    long long maxIter = 250;
    double dx = 3.0 / 1000.0;
    double dy = 3.0 / 1000.0;

    long long totalIter = 0;
    long long py = 0;
    double cy = -1.5;

    while (py < height) {
        long long px = 0;
        double cx = -2.0;
        while (px < width) {
            double zx = 0.0;
            double zy = 0.0;
            long long iter = 0;
            while (iter < maxIter && zx * zx + zy * zy <= 4.0) {
                double tmp = zx * zx - zy * zy + cx;
                zy = 2.0 * zx * zy + cy;
                zx = tmp;
                iter = iter + 1;
            }
            totalIter = totalIter + iter;
            cx = cx + dx;
            px = px + 1;
        }
        cy = cy + dy;
        py = py + 1;
    }

    std::cout << totalIter << "\n";
    return 0;
}
