//go:build ignore

// Counts primes below 2,000,000 by trial division. Mirrors primes.nio in Go.
package main

import "fmt"

func main() {
	limit := 2000000
	count := 1 // 2 is prime
	i := 3

	for i < limit {
		isPrime := true
		j := 3
		for j*j <= i && isPrime {
			if i%j == 0 {
				isPrime = false
			}
			j = j + 2
		}
		if isPrime {
			count = count + 1
		}
		i = i + 2
	}

	fmt.Println(count)
}
