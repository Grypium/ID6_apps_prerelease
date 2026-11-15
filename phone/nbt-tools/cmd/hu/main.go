// hu 'COMMAND': run one shell command on the head unit (see package hulink for setup).
package main

import (
	"fmt"
	"os"

	"nbt-tools/hulink"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: hu 'command'")
		os.Exit(2)
	}
	cl, err := hulink.Dial()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer cl.Close()
	s, err := cl.NewSession()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	// Piped input (e.g. a file to write with "cat > path") goes to the remote command.
	if st, err := os.Stdin.Stat(); err == nil && st.Mode()&os.ModeCharDevice == 0 {
		s.Stdin = os.Stdin
	}
	out, err := s.CombinedOutput(os.Args[1])
	os.Stdout.Write(out)
	if err != nil {
		fmt.Fprintln(os.Stderr, "run:", err)
		os.Exit(1)
	}
}
