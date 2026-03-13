BINARY=mtprotor

.PHONY: build test fmt

build:
	go build -o bin/$(BINARY) ./cmd/mtprotor

test:
	go test ./...

fmt:
	gofmt -w ./cmd ./internal
