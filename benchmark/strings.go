//go:build ignore

// Mirrors strings.nio in Go. Go strings are immutable too, so every "+"
// allocates a fresh string.
package main

import "fmt"

func main() {
	n := 100000
	target := "abababababababababababababababab"
	hits := 0
	i := 0

	for i < n {
		s := ""
		j := 0
		for j < 16 {
			s = s + "ab"
			j = j + 1
		}
		if s == target {
			hits = hits + 1
		}
		i = i + 1
	}

	fmt.Println(hits)
}
