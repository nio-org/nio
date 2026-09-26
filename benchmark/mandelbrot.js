// Mandelbrot escape-time over a 1000x1000 grid. Mirrors mandelbrot.nio:
// float accumulators for the coordinates, and no break.
const width = 1000;
const height = 1000;
const maxIter = 250;
const dx = 3.0 / 1000.0;
const dy = 3.0 / 1000.0;

let totalIter = 0;
let py = 0;
let cy = -1.5;

while (py < height) {
    let px = 0;
    let cx = -2.0;
    while (px < width) {
        let zx = 0.0;
        let zy = 0.0;
        let iter = 0;
        while (iter < maxIter && zx * zx + zy * zy <= 4.0) {
            const tmp = zx * zx - zy * zy + cx;
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

console.log(totalIter);
