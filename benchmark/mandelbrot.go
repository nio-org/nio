//go:build ignore

// Mandelbrot escape time over a 1000x1000 grid, as in mandelbrot.nio. The
// coordinates are float accumulators, and the loops have no break.
//
// Keep the float64(...) conversions around each product. The Go spec lets an
// implementation fuse a*b + c into one FMA, and the arm64 backend does this.
// The conversion forces the same intermediate rounding as the other versions.
// Without it, the checksum is different.
package main

import "fmt"

func main() {
	width := 1000
	height := 1000
	maxIter := 250
	dx := 3.0 / 1000.0
	dy := 3.0 / 1000.0

	totalIter := 0
	py := 0
	cy := -1.5

	for py < height {
		px := 0
		cx := -2.0
		for px < width {
			zx := 0.0
			zy := 0.0
			iter := 0
			for iter < maxIter && float64(zx*zx)+float64(zy*zy) <= 4.0 {
				tmp := float64(zx*zx) - float64(zy*zy) + cx
				zy = float64(2.0*zx*zy) + cy
				zx = tmp
				iter = iter + 1
			}
			totalIter = totalIter + iter
			cx = cx + dx
			px = px + 1
		}
		cy = cy + dy
		py = py + 1
	}

	fmt.Println(totalIter)
}
