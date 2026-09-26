//go:build ignore

// The Go version of files.nio. Go strings are immutable, as in Nio. Go's
// regexp is an NFA simulation, as is Nio's engine.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
)

func main() {
	n := 1000
	lines := 400

	doc := ""
	k := 0
	for k < lines {
		doc = doc + "field_" + strconv . Itoa(k) + " = value_" + strconv.Itoa(k) + "\n"
		k = k + 1
	}
	doc = doc + "counter = 0000\n"

	dir, err := os.MkdirTemp("", "nio-bench-files-")
	if err != nil {
		os.Exit(1)
	}
	file := filepath.Join(dir, "data.txt")
	os.WriteFile(file, []byte(doc), 0o644)

	re := regexp.MustCompile("counter = [0-9]{4}")
	checksum := 0
	i := 0

	for i < n {
		bytes, _ := os.ReadFile(file)
		text := string(bytes)
		at := re.FindStringIndex(text)[0]
		checksum = checksum + at

		v, _ := strconv.Atoi(text[at+10 : at+14])
		v = v + 1
		num := fmt.Sprintf("%04d", v)
		edited := text[:at+10] + num + text[at+14:]

		os.WriteFile(file, []byte(edited), 0o644)
		i = i + 1
	}

	bytes, _ := os.ReadFile(file)
	text := string(bytes)
	at := re.FindStringIndex(text)[0]
	counter, _ := strconv.Atoi(text[at+10 : at+14])

	os.RemoveAll(dir)
	fmt.Println(checksum + counter)
}
