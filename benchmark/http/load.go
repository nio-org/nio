// The load generator, shared by every server in this suite. It speaks HTTP/1.1
// over raw TCP with keep-alive, and sends one request at a time per connection.
// It does not pipeline, because a pipelined client measures the server's read
// buffering and not its request path.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

func main() {
	addr := flag.String("addr", "", "host:port to hit")
	path := flag.String("path", "/plaintext", "request target")
	conns := flag.Int("conns", 1, "concurrent connections")
	total := flag.Int("requests", 20000, "requests in total, split across connections")
	warmup := flag.Int("warmup", 2000, "requests to run and discard first")
	flag.Parse()

	if *addr == "" {
		fmt.Fprintln(os.Stderr, "load: -addr is required")
		os.Exit(2)
	}

	host := *addr
	if i := strings.LastIndex(host, ":"); i >= 0 {
		host = host[:i]
	}
	req := []byte("GET " + *path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n")

	// The warm-up pass is thrown away: the first requests on a fresh server pay
	// for its first allocations, and on Node for the JIT.
	if *warmup > 0 {
		if _, err := drive(*addr, req, 1, *warmup); err != nil {
			fmt.Fprintln(os.Stderr, "load: warmup:", err)
			os.Exit(1)
		}
	}

	body, err := drive(*addr, req, *conns, *total)
	if err != nil {
		fmt.Fprintln(os.Stderr, "load:", err)
		os.Exit(1)
	}

	// Only the requests are timed. Opening the connections is not part of a
	// keep-alive server's steady state, and at high counts it is mostly the
	// kernel's listen queue at work.
	clients, err := open(*addr, req, *conns)
	if err != nil {
		fmt.Fprintln(os.Stderr, "load:", err)
		os.Exit(1)
	}
	start := time.Now()
	if _, err := run(clients, req, perConn(*total, *conns)); err != nil {
		fmt.Fprintln(os.Stderr, "load:", err)
		os.Exit(1)
	}
	elapsed := time.Since(start)
	closeAll(clients)

	fmt.Printf("body %s\n", body)
	fmt.Printf("rps %.0f\n", float64(*total)/elapsed.Seconds())
}

// A connection and the reader over it, kept open between requests.
type client struct {
	c net.Conn
	r *bufio.Reader
}

// How many connections are opened at once. macOS caps every listen queue at
// 128 (kern.ipc.somaxconn) and resets connections that arrive when it is full,
// so a burst of a thousand connects fails on every server alike. A batch well
// under the cap never fills it.
const dialBatch = 64

// drive opens `conns` connections, runs `total` requests split evenly over
// them, and answers one response body, which the runner compares between
// servers.
func drive(addr string, req []byte, conns, total int) (string, error) {
	clients, err := open(addr, req, conns)
	if err != nil {
		return "", err
	}
	defer closeAll(clients)
	return run(clients, req, perConn(total, conns))
}

func perConn(total, conns int) int {
	if total/conns == 0 {
		return 1
	}
	return total / conns
}

// open dials `n` connections, dialBatch at a time, and sends one request on
// each. The response proves that the server accepted the connection. A
// completed dial does not prove it, because the kernel completes the
// handshake before accept.
func open(addr string, req []byte, n int) ([]*client, error) {
	clients := make([]*client, n)
	errs := make([]error, n)
	for lo := 0; lo < n; lo += dialBatch {
		hi := lo + dialBatch
		if hi > n {
			hi = n
		}
		var wg sync.WaitGroup
		for i := lo; i < hi; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				clients[i], errs[i] = dial(addr, req)
			}(i)
		}
		wg.Wait()
		for _, err := range errs[lo:hi] {
			if err != nil {
				closeAll(clients)
				return nil, err
			}
		}
	}
	return clients, nil
}

func dial(addr string, req []byte) (*client, error) {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	if tcp, ok := c.(*net.TCPConn); ok {
		tcp.SetNoDelay(true)
	}
	cl := &client{c: c, r: bufio.NewReaderSize(c, 16<<10)}
	if _, err := roundTrip(cl, req); err != nil {
		c.Close()
		return nil, fmt.Errorf("first request: %w", err)
	}
	return cl, nil
}

func closeAll(clients []*client) {
	for _, cl := range clients {
		if cl != nil {
			cl.c.Close()
		}
	}
}

// run does `per` sequential round trips on every connection at once.
func run(clients []*client, req []byte, per int) (string, error) {
	var wg sync.WaitGroup
	bodies := make([]string, len(clients))
	errs := make([]error, len(clients))
	for i, cl := range clients {
		wg.Add(1)
		go func(i int, cl *client) {
			defer wg.Done()
			for n := 0; n < per; n++ {
				body, err := roundTrip(cl, req)
				if err != nil {
					errs[i] = fmt.Errorf("request %d: %w", n, err)
					return
				}
				bodies[i] = body
			}
		}(i, cl)
	}
	wg.Wait()
	for _, err := range errs {
		if err != nil {
			return "", err
		}
	}
	return bodies[0], nil
}

func roundTrip(cl *client, req []byte) (string, error) {
	if _, err := cl.c.Write(req); err != nil {
		return "", err
	}
	return readResponse(cl.r)
}

// readResponse reads one whole response. Only Content-Length framing is
// handled, which is all any server in this suite sends.
func readResponse(r *bufio.Reader) (string, error) {
	length := -1
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return "", err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			break
		}
		if i := strings.IndexByte(line, ':'); i > 0 {
			if strings.EqualFold(strings.TrimSpace(line[:i]), "content-length") {
				length, err = strconv.Atoi(strings.TrimSpace(line[i+1:]))
				if err != nil {
					return "", err
				}
			}
		}
	}
	if length < 0 {
		return "", fmt.Errorf("no Content-Length in response")
	}
	buf := make([]byte, length)
	for read := 0; read < length; {
		n, err := r.Read(buf[read:])
		if err != nil {
			return "", err
		}
		read += n
	}
	return string(buf), nil
}
