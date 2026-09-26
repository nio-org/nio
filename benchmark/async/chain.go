//go:build ignore

// A million sequential goroutine round trips through one channel. Mirrors
// chain.nio in Go.
package main

import "fmt"

func bump(x int, c chan int) {
	c <- x + 1
}

func main() {
	n := 1000000
	c := make(chan int)
	acc := 0
	for i := 0; i < n; i++ {
		go bump(i, c)
		acc = (acc + <-c) % 1000000
	}
	fmt.Println(acc)
}
