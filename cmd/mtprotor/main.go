package main

import (
	"os"

	"github.com/example/mtprotor/internal/app"
)

func main() {
	os.Exit(app.Run(os.Args[1:]))
}
