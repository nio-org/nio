// HTTP request parsing through Go's net/http. http.ReadRequest is a tuned
// production parser, so the comparison with parse.nio is not like for like.
package main

import (
	"bufio"
	"bytes"
	"fmt"
	"net/http"
	"os"
)

const iterations = 200000

func main() {
	block := "GET /users/42?a=1&b=2 HTTP/1.1\r\n" +
		"Host: example.com\r\n" +
		"User-Agent: bench/1\r\n" +
		"Accept: */*\r\n" +
		"Accept-Encoding: gzip, deflate\r\n" +
		"Connection: keep-alive\r\n" +
		"Content-Length: 0\r\n" +
		"\r\n"
	raw := []byte(block)

	sum := 0
	reader := bufio.NewReader(bytes.NewReader(nil))
	for i := 0; i < iterations; i++ {
		reader.Reset(bytes.NewReader(raw))
		req, err := http.ReadRequest(reader)
		if err != nil {
			fmt.Fprintln(os.Stderr, "parse:", err)
			os.Exit(1)
		}
		// Add 1 for Host. ReadRequest moves it out of the header map.
		sum += len(req.URL.Path) + len(req.URL.RawQuery) + len(req.Header) + 1
	}
	fmt.Println(sum)
}
