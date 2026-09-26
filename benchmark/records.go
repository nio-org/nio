//go:build ignore

// The Go version of records.nio. Each point and segment is a separate heap
// block. A segment that leaves the ring becomes garbage for the collector.
package main

import "fmt"

type point struct {
	x int
	y int
}

type segment struct {
	a *point
	b *point
}

func makeSegment(i int) *segment {
	return &segment{
		a: &point{x: i % 1000, y: (i * 7) % 1000},
		b: &point{x: (i * 13) % 1000, y: (i * 31) % 1000},
	}
}

func length2(s *segment) int {
	dx := s.b.x - s.a.x
	dy := s.b.y - s.a.y
	return dx*dx + dy*dy
}

func main() {
	n := 2000000
	recent := []*segment{makeSegment(0), makeSegment(0), makeSegment(0), makeSegment(0)}
	checksum := 0
	i := 0

	for i < n {
		s := makeSegment(i)
		checksum = (checksum + length2(s)) % 1000000007
		recent[i%4] = s
		i = i + 1
	}

	fmt.Println(checksum)
	fmt.Println(length2(recent[n%4]))
}
