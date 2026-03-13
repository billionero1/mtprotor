package worker

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/example/mtprotor/internal/config"
)

type Instance struct {
	SecretID string
	Secret   string
	Port     int
	Addr     string
	Started  time.Time

	mode   string
	cmd    *exec.Cmd
	cancel context.CancelFunc
	done   chan error
}

type Manager struct {
	cfg    config.WorkerConfig
	logger *slog.Logger

	mu        sync.Mutex
	instances map[string]*Instance
	usedPorts map[int]string
}

func NewManager(cfg config.WorkerConfig, logger *slog.Logger) *Manager {
	return &Manager{
		cfg:       cfg,
		logger:    logger,
		instances: map[string]*Instance{},
		usedPorts: map[int]string{},
	}
}

func (m *Manager) Start(secretID, secret string) (*Instance, error) {
	m.mu.Lock()
	if inst, ok := m.instances[secretID]; ok {
		m.mu.Unlock()
		return inst, nil
	}
	port, err := m.allocatePortLocked(secretID)
	if err != nil {
		m.mu.Unlock()
		return nil, err
	}
	m.mu.Unlock()

	addr := net.JoinHostPort("127.0.0.1", strconv.Itoa(port))
	inst := &Instance{
		SecretID: secretID,
		Secret:   secret,
		Port:     port,
		Addr:     addr,
		Started:  time.Now().UTC(),
		mode:     m.cfg.Mode,
		done:     make(chan error, 1),
	}

	switch m.cfg.Mode {
	case "command":
		if err := m.startCommandMode(inst); err != nil {
			m.releasePort(secretID, port)
			return nil, err
		}
	case "builtin":
		if err := m.startBuiltinMode(inst); err != nil {
			m.releasePort(secretID, port)
			return nil, err
		}
	default:
		m.releasePort(secretID, port)
		return nil, fmt.Errorf("unsupported worker mode %q", m.cfg.Mode)
	}

	m.mu.Lock()
	m.instances[secretID] = inst
	m.mu.Unlock()

	m.logger.Info("worker started", "secret_id", secretID, "addr", addr, "mode", m.cfg.Mode)
	return inst, nil
}

func (m *Manager) Stop(secretID string) error {
	m.mu.Lock()
	inst, ok := m.instances[secretID]
	if !ok {
		m.mu.Unlock()
		return nil
	}
	delete(m.instances, secretID)
	delete(m.usedPorts, inst.Port)
	m.mu.Unlock()

	var stopErr error
	switch inst.mode {
	case "command":
		stopErr = stopCommand(inst, time.Duration(m.cfg.StopGracePeriodSeconds)*time.Second)
	case "builtin":
		if inst.cancel != nil {
			inst.cancel()
		}
		select {
		case <-inst.done:
		case <-time.After(time.Duration(m.cfg.StopGracePeriodSeconds) * time.Second):
			stopErr = errors.New("builtin worker stop timeout")
		}
	}
	if stopErr != nil {
		m.logger.Error("worker stop failed", "secret_id", secretID, "err", stopErr)
		return stopErr
	}
	m.logger.Info("worker stopped", "secret_id", secretID)
	return nil
}

func (m *Manager) Addr(secretID string) (string, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	inst, ok := m.instances[secretID]
	if !ok {
		return "", false
	}
	return inst.Addr, true
}

func (m *Manager) StopAll() {
	m.mu.Lock()
	ids := make([]string, 0, len(m.instances))
	for id := range m.instances {
		ids = append(ids, id)
	}
	m.mu.Unlock()
	for _, id := range ids {
		_ = m.Stop(id)
	}
}

func (m *Manager) startCommandMode(inst *Instance) error {
	args := make([]string, 0, len(m.cfg.Args))
	for _, arg := range m.cfg.Args {
		replaced := strings.ReplaceAll(arg, "{{port}}", strconv.Itoa(inst.Port))
		replaced = strings.ReplaceAll(replaced, "{{secret}}", inst.Secret)
		replaced = strings.ReplaceAll(replaced, "{{id}}", inst.SecretID)
		args = append(args, replaced)
	}

	cmd := exec.Command(m.cfg.Command, args...)
	env := os.Environ()
	for k, v := range m.cfg.Env {
		env = append(env, k+"="+v)
	}
	cmd.Env = env

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("stdout pipe: %w", err)
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return fmt.Errorf("stderr pipe: %w", err)
	}

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("start worker command: %w", err)
	}
	inst.cmd = cmd

	go m.streamWorkerLogs(inst, "stdout", stdout)
	go m.streamWorkerLogs(inst, "stderr", stderr)
	go func() {
		inst.done <- cmd.Wait()
		close(inst.done)
	}()

	timeout := time.Duration(m.cfg.StartTimeoutMillis) * time.Millisecond
	if err := waitForTCP(inst.Addr, timeout); err != nil {
		_ = stopCommand(inst, time.Duration(m.cfg.StopGracePeriodSeconds)*time.Second)
		return fmt.Errorf("worker did not become ready: %w", err)
	}
	return nil
}

func (m *Manager) startBuiltinMode(inst *Instance) error {
	ctx, cancel := context.WithCancel(context.Background())
	inst.cancel = cancel

	ln, err := net.Listen("tcp", inst.Addr)
	if err != nil {
		cancel()
		return fmt.Errorf("start builtin worker listen: %w", err)
	}

	go func() {
		defer close(inst.done)
		defer ln.Close()
		for {
			conn, err := ln.Accept()
			if err != nil {
				select {
				case <-ctx.Done():
					inst.done <- nil
					return
				default:
					inst.done <- err
					return
				}
			}
			go m.handleBuiltinConn(ctx, inst, conn)
		}
	}()

	if err := waitForTCP(inst.Addr, time.Duration(m.cfg.StartTimeoutMillis)*time.Millisecond); err != nil {
		cancel()
		return fmt.Errorf("builtin worker did not become ready: %w", err)
	}
	return nil
}

func (m *Manager) handleBuiltinConn(ctx context.Context, inst *Instance, in net.Conn) {
	defer in.Close()

	out, err := net.DialTimeout("tcp", m.cfg.BuiltinUpstreamAddr, 5*time.Second)
	if err != nil {
		m.logger.Error("builtin upstream dial failed", "secret_id", inst.SecretID, "err", err)
		return
	}
	defer out.Close()

	relay(ctx, in, out)
}

func (m *Manager) streamWorkerLogs(inst *Instance, streamName string, r io.ReadCloser) {
	defer r.Close()
	s := bufio.NewScanner(r)
	for s.Scan() {
		m.logger.Info("worker log", "secret_id", inst.SecretID, "stream", streamName, "line", s.Text())
	}
}

func (m *Manager) allocatePortLocked(secretID string) (int, error) {
	for p := m.cfg.PortRangeStart; p <= m.cfg.PortRangeEnd; p++ {
		if _, used := m.usedPorts[p]; used {
			continue
		}
		if !isPortFree(p) {
			continue
		}
		m.usedPorts[p] = secretID
		return p, nil
	}
	return 0, errors.New("no free worker port in configured range")
}

func (m *Manager) releasePort(secretID string, port int) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if owner, ok := m.usedPorts[port]; ok && owner == secretID {
		delete(m.usedPorts, port)
	}
}

func waitForTCP(addr string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 300*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			return nil
		}
		time.Sleep(80 * time.Millisecond)
	}
	return fmt.Errorf("timeout waiting for tcp %s", addr)
}

func isPortFree(port int) bool {
	ln, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)))
	if err != nil {
		return false
	}
	_ = ln.Close()
	return true
}

func stopCommand(inst *Instance, grace time.Duration) error {
	if inst.cmd == nil || inst.cmd.Process == nil {
		return nil
	}
	if err := inst.cmd.Process.Signal(syscall.SIGTERM); err != nil {
		_ = inst.cmd.Process.Kill()
		return fmt.Errorf("signal worker: %w", err)
	}

	select {
	case err := <-inst.done:
		return err
	case <-time.After(grace):
		_ = inst.cmd.Process.Kill()
		select {
		case err := <-inst.done:
			return err
		case <-time.After(2 * time.Second):
			return errors.New("worker kill timeout")
		}
	}
}

func relay(ctx context.Context, a net.Conn, b net.Conn) {
	errC := make(chan error, 2)
	copyFn := func(dst net.Conn, src net.Conn) {
		_, err := io.Copy(dst, src)
		errC <- err
	}
	go copyFn(a, b)
	go copyFn(b, a)
	select {
	case <-ctx.Done():
	case <-errC:
	}
	_ = a.Close()
	_ = b.Close()
}
