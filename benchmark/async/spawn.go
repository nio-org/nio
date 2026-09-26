//go:build ignore

// 100 waves of 10,000 concurrent goroutines. Mirrors spawn.nio in Go.
package main

import (
	"fmt"
	"sync"
)

func main() {
	total := 0
	for w := 0; w < 100; w++ {
		results := make([]int, 10000)
		var wg sync.WaitGroup
		wg.Add(10000)
		for i := 0; i < 10000; i++ {
			go func(i int) {
				results[i] = (w + i) % 7
				wg.Done()
			}(i)
		}
		wg.Wait()
		for _, r := range results {
			total = (total + r) % 1000000
		}
	}
	fmt.Println(total)
}
