// Mandelbrot escape time over a 1000x1000 grid, as in mandelbrot.nio. The
// coordinates are float accumulators, and the loops have no break. Java does
// not fuse a*b + c into one FMA, so the checksum is the same as the others.
class Main {
    public static void main(String[] args) {
        long width = 1000;
        long height = 1000;
        long maxIter = 250;
        double dx = 3.0 / 1000.0;
        double dy = 3.0 / 1000.0;

        long totalIter = 0;
        long py = 0;
        double cy = -1.5;

        while (py < height) {
            long px = 0;
            double cx = -2.0;
            while (px < width) {
                double zx = 0.0;
                double zy = 0.0;
                long iter = 0;
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

        System.out.println(totalIter);
    }
}
