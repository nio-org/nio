# Mandelbrot escape time over a 1000x1000 grid. It is mandelbrot.nio in
# Python: float accumulators for the coordinates, and no break.
width = 1000
height = 1000
maxIter = 250
dx = 3.0 / 1000.0
dy = 3.0 / 1000.0

totalIter = 0
py = 0
cy = -1.5

while py < height:
    px = 0
    cx = -2.0
    while px < width:
        zx = 0.0
        zy = 0.0
        iter = 0
        while iter < maxIter and zx * zx + zy * zy <= 4.0:
            tmp = zx * zx - zy * zy + cx
            zy = 2.0 * zx * zy + cy
            zx = tmp
            iter = iter + 1
        totalIter = totalIter + iter
        cx = cx + dx
        px = px + 1
    cy = cy + dy
    py = py + 1

print(totalIter)
