//go:build ignore

// 100,000,000 iterations of a nested loop. Mirrors loops.nio in Go.
package main

import "fmt"

func main() {
	u := 10
	r := 5000
	var a [10000]int
	i := 0

	for i < 10000 {
		j := 0
		for j < 10000 {
			a[i] = a[i] + j%u
			j = j + 1
		}
		a[i] = a[i] + r
		i = i + 1
	}

	fmt.Println(a[r])
}
