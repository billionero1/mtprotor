package runtime

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/example/mtprotor/internal/config"
	"github.com/example/mtprotor/internal/handshake"
)

func TestHotRemoveKeepsActiveConnection(t *testing.T) {
	t.Parallel()
	upstream := startMockUpstream(t)

	cfg := config.Config{
		ListenAddr:             "127.0.0.1:0",
		AdminSocket:            filepath.Join(t.TempDir(), "admin.sock"),
		StateFile:              filepath.Join(t.TempDir(), "state.json"),
		LogLevel:               "error",
		HandshakeTimeoutMillis: 2000,
		DialTimeoutMillis:      2000,
		DropOnDisable:          false,
		Worker: config.WorkerConfig{
			Mode:                   "builtin",
			PortRangeStart:         31000,
			PortRangeEnd:           31100,
			StartTimeoutMillis:     1000,
			StopGracePeriodSeconds: 1,
			BuiltinUpstreamAddr:    upstream,
		},
	}

	rt := New(cfg, testLogger())
	if err := rt.Start(); err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "operation not permitted") {
			t.Skipf("skipping integration test in restricted sandbox: %v", err)
		}
		t.Fatalf("start runtime: %v", err)
	}
	defer func() {
		_ = rt.Shutdown(context.Background())
	}()

	item, err := rt.AddSecret(AddSecretInput{
		Secret:  "00112233445566778899aabbccddeeff",
		Label:   "main",
		Enabled: true,
	})
	if err != nil {
		t.Fatalf("add secret: %v", err)
	}

	conn := mustDialClient(t, rt.ProxyAddr())
	defer conn.Close()
	if err := sendHandshake(conn, "00112233445566778899aabbccddeeff"); err != nil {
		t.Fatalf("send handshake: %v", err)
	}
	assertEcho(t, conn, []byte("alpha"))

	if err := rt.RemoveSecret(item.ID); err != nil {
		t.Fatalf("remove secret: %v", err)
	}

	assertEcho(t, conn, []byte("beta"))

	conn2 := mustDialClient(t, rt.ProxyAddr())
	defer conn2.Close()
	if err := sendHandshake(conn2, "00112233445566778899aabbccddeeff"); err != nil {
		t.Fatalf("send handshake 2: %v", err)
	}
	_, _ = conn2.Write([]byte("gamma"))
	_ = conn2.SetReadDeadline(time.Now().Add(700 * time.Millisecond))
	b := make([]byte, 5)
	_, err = io.ReadFull(conn2, b)
	if err == nil {
		t.Fatalf("expected new connection to be rejected after remove")
	}
}

func TestMultiSecretHotAddDoesNotDropExistingConnection(t *testing.T) {
	t.Parallel()
	upstream := startMockUpstream(t)

	cfg := config.Config{
		ListenAddr:             "127.0.0.1:0",
		AdminSocket:            filepath.Join(t.TempDir(), "admin.sock"),
		StateFile:              filepath.Join(t.TempDir(), "state.json"),
		LogLevel:               "error",
		HandshakeTimeoutMillis: 2000,
		DialTimeoutMillis:      2000,
		DropOnDisable:          false,
		Worker: config.WorkerConfig{
			Mode:                   "builtin",
			PortRangeStart:         31400,
			PortRangeEnd:           31500,
			StartTimeoutMillis:     1000,
			StopGracePeriodSeconds: 1,
			BuiltinUpstreamAddr:    upstream,
		},
	}

	rt := New(cfg, testLogger())
	if err := rt.Start(); err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "operation not permitted") {
			t.Skipf("skipping integration test in restricted sandbox: %v", err)
		}
		t.Fatalf("start runtime: %v", err)
	}
	defer func() {
		_ = rt.Shutdown(context.Background())
	}()

	first, err := rt.AddSecret(AddSecretInput{
		Secret:  "11111111111111111111111111111111",
		Label:   "first",
		Enabled: true,
	})
	if err != nil {
		t.Fatalf("add first secret: %v", err)
	}
	_ = first

	conn1 := mustDialClient(t, rt.ProxyAddr())
	defer conn1.Close()
	if err := sendHandshake(conn1, "11111111111111111111111111111111"); err != nil {
		t.Fatalf("send first handshake: %v", err)
	}
	assertEcho(t, conn1, []byte("keepalive-1"))

	second, err := rt.AddSecret(AddSecretInput{
		Secret:  "22222222222222222222222222222222",
		Label:   "second",
		Enabled: true,
	})
	if err != nil {
		t.Fatalf("add second secret: %v", err)
	}
	_ = second

	// Existing stream must survive runtime secret update.
	assertEcho(t, conn1, []byte("keepalive-2"))

	conn2 := mustDialClient(t, rt.ProxyAddr())
	defer conn2.Close()
	if err := sendHandshake(conn2, "22222222222222222222222222222222"); err != nil {
		t.Fatalf("send second handshake: %v", err)
	}
	assertEcho(t, conn2, []byte("new-secret-ok"))
}

func TestStateRestoreAfterRestart(t *testing.T) {
	t.Parallel()
	upstream := startMockUpstream(t)
	stateFile := filepath.Join(t.TempDir(), "state.json")

	cfg := config.Config{
		ListenAddr:             "127.0.0.1:0",
		AdminSocket:            filepath.Join(t.TempDir(), "admin.sock"),
		StateFile:              stateFile,
		LogLevel:               "error",
		HandshakeTimeoutMillis: 2000,
		DialTimeoutMillis:      2000,
		DropOnDisable:          false,
		Worker: config.WorkerConfig{
			Mode:                   "builtin",
			PortRangeStart:         31200,
			PortRangeEnd:           31300,
			StartTimeoutMillis:     1000,
			StopGracePeriodSeconds: 1,
			BuiltinUpstreamAddr:    upstream,
		},
	}

	r1 := New(cfg, testLogger())
	if err := r1.Start(); err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "operation not permitted") {
			t.Skipf("skipping integration test in restricted sandbox: %v", err)
		}
		t.Fatalf("start first runtime: %v", err)
	}
	if _, err := r1.AddSecret(AddSecretInput{
		Secret:  "11223344556677889900aabbccddeeff",
		Enabled: true,
	}); err != nil {
		t.Fatalf("add secret: %v", err)
	}
	if err := r1.Shutdown(context.Background()); err != nil {
		t.Fatalf("shutdown first runtime: %v", err)
	}

	r2 := New(cfg, testLogger())
	if err := r2.Start(); err != nil {
		t.Fatalf("start second runtime: %v", err)
	}
	defer func() {
		_ = r2.Shutdown(context.Background())
	}()

	if got := len(r2.ListSecrets()); got != 1 {
		t.Fatalf("expected restored secret count=1, got=%d", got)
	}

	conn := mustDialClient(t, r2.ProxyAddr())
	defer conn.Close()
	if err := sendHandshake(conn, "11223344556677889900aabbccddeeff"); err != nil {
		t.Fatalf("send handshake: %v", err)
	}
	assertEcho(t, conn, []byte("ok"))
}

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func startMockUpstream(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "operation not permitted") {
			t.Skipf("skipping integration test in restricted sandbox: %v", err)
		}
		t.Fatalf("start mock upstream: %v", err)
	}
	t.Cleanup(func() { _ = ln.Close() })

	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				if errors.Is(err, net.ErrClosed) {
					return
				}
				return
			}
			go func(conn net.Conn) {
				defer conn.Close()
				first := make([]byte, 64)
				if _, err := io.ReadFull(conn, first); err != nil {
					return
				}
				_, _ = io.Copy(conn, conn)
			}(c)
		}
	}()

	return ln.Addr().String()
}

func mustDialClient(t *testing.T, addr string) net.Conn {
	t.Helper()
	c, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial client: %v", err)
	}
	return c
}

func sendHandshake(conn net.Conn, secret string) error {
	p, err := handshake.BuildClientPreamble(secret, 0xdddddddd, 2)
	if err != nil {
		return err
	}
	_, err = conn.Write(p)
	return err
}

func assertEcho(t *testing.T, conn net.Conn, payload []byte) {
	t.Helper()
	if _, err := conn.Write(payload); err != nil {
		t.Fatalf("write payload: %v", err)
	}
	out := make([]byte, len(payload))
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := io.ReadFull(conn, out); err != nil {
		t.Fatalf("read echo: %v", err)
	}
	if string(out) != string(payload) {
		t.Fatalf("echo mismatch: got=%q want=%q", string(out), string(payload))
	}
}
