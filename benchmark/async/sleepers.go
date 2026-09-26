//go:build ignore

// 10,000 goroutines sleeping one millisecond at once. Mirrors sleepers.nio
// in Go.
package main

import (
	"fmt"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	var woken atomic.Int64
	var wg sync.WaitGroup
	wg.Add(10000)
	for i := 0; i < 10000; i++ {
		go func() {
			time.Sleep(time.Millisecond)
			woken.Add(1)
			wg.Done()
		}()
	}
	wg.Wait()
	fmt.Println(woken.Load())
}
