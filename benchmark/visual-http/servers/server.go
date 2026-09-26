// The Go server for the visual HTTP benchmark. It has the same routes as
// server.nio and uses net/http and ServeMux. net/http uses all cores, so the
// runner also has the go1 target, which runs this binary with GOMAXPROCS=1.
package main

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"strings"
)

type message struct {
	Message string `json:"message"`
}

type item struct {
	ID     int     `json:"id"`
	Name   string  `json:"name"`
	Price  float64 `json:"price"`
	Qty    int     `json:"qty"`
	Active bool    `json:"active"`
}

type payload struct {
	Items []item `json:"items"`
}

type summary struct {
	Count int     `json:"count"`
	Total float64 `json:"total"`
}

func main() {
	mux := http.NewServeMux()

	mux.HandleFunc("/plaintext", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		fmt.Fprint(w, "Hello, World!")
	})

	mux.HandleFunc("/json", func(w http.ResponseWriter, r *http.Request) {
		// json.Encoder appends a newline that the other servers do not send.
		b, _ := json.Marshal(message{Message: "Hello, World!"})
		w.Header().Set("Content-Type", "application/json")
		w.Write(b)
	})

	mux.HandleFunc("/users/", func(w http.ResponseWriter, r *http.Request) {
		id := strings.TrimPrefix(r.URL.Path, "/users/")
		if id == "" {
			http.Error(w, "missing id", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		fmt.Fprint(w, "user "+id)
	})

	mux.HandleFunc("/data", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		var p payload
		if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
			http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
			return
		}
		total := 0.0
		for _, it := range p.Items {
			total += it.Price
		}
		b, _ := json.Marshal(summary{Count: len(p.Items), Total: total})
		w.Header().Set("Content-Type", "application/json")
		w.Write(b)
	})

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println(ln.Addr().(*net.TCPAddr).Port)
	os.Stdout.Sync()

	srv := &http.Server{Handler: mux}
	if err := srv.Serve(ln); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
