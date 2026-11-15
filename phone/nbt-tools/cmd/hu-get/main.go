// hu-get REMOTE LOCAL: copy a file from the head unit over ONE SSH connection,
// in verified chunks fetched in parallel (the unit's old sshd truncates long streams).
// Connection setup: see package hulink.
package main

import (
	"bytes"
	"fmt"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/ssh"

	"nbt-tools/hulink"
)

const (
	chunk   = 256 << 10
	workers = 4
)

func run(cl *ssh.Client, cmd string) ([]byte, error) {
	s, err := cl.NewSession()
	if err != nil {
		return nil, err
	}
	defer s.Close()
	var out bytes.Buffer
	s.Stdout = &out
	err = s.Run(cmd)
	return out.Bytes(), err
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: hu-get REMOTE_FILE LOCAL_FILE")
		os.Exit(2)
	}
	remote, local := os.Args[1], os.Args[2]
	cl, err := hulink.Dial()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer cl.Close()
	ls, err := run(cl, "ls -ln "+remote)
	if err != nil {
		panic(fmt.Sprint("ls: ", err))
	}
	size, err := strconv.Atoi(strings.Fields(string(ls))[4])
	if err != nil {
		panic(err)
	}
	n := (size + chunk - 1) / chunk
	buf := make([]byte, size)
	jobs := make(chan int)
	var wg sync.WaitGroup
	var mu sync.Mutex
	done, failed := 0, 0
	start := time.Now()
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				want := chunk
				if i == n-1 {
					want = size - i*chunk
				}
				ok := false
				for try := 0; try < 6 && !ok; try++ {
					b, _ := run(cl, fmt.Sprintf("dd if=%s bs=%d skip=%d count=1 2>/dev/null", remote, chunk, i))
					if len(b) == want {
						copy(buf[i*chunk:], b)
						ok = true
					}
				}
				mu.Lock()
				if ok {
					done++
				} else {
					failed++
				}
				if done%20 == 0 {
					el := time.Since(start).Seconds()
					fmt.Fprintf(os.Stderr, "%d/%d chunks, %.0f KB/s\n", done, n, float64(done*chunk)/1024/el)
				}
				mu.Unlock()
			}
		}()
	}
	for i := 0; i < n; i++ {
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	if failed > 0 {
		fmt.Fprintf(os.Stderr, "%d chunks failed\n", failed)
		os.Exit(1)
	}
	if err := os.WriteFile(local, buf, 0o600); err != nil {
		panic(err)
	}
	fmt.Fprintf(os.Stderr, "copied %d bytes in %s\n", size, time.Since(start).Round(time.Second))
}
