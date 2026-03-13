package config

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

type WorkerConfig struct {
	Mode                   string            `json:"mode"`
	Command                string            `json:"command"`
	Args                   []string          `json:"args"`
	Env                    map[string]string `json:"env"`
	PortRangeStart         int               `json:"port_range_start"`
	PortRangeEnd           int               `json:"port_range_end"`
	StartTimeoutMillis     int               `json:"start_timeout_ms"`
	StopGracePeriodSeconds int               `json:"stop_grace_period_seconds"`
	BuiltinUpstreamAddr    string            `json:"builtin_upstream_addr"`
}

type Config struct {
	ListenAddr             string       `json:"listen_addr"`
	AdminSocket            string       `json:"admin_socket"`
	StateFile              string       `json:"state_file"`
	LogLevel               string       `json:"log_level"`
	HandshakeTimeoutMillis int          `json:"handshake_timeout_ms"`
	DialTimeoutMillis      int          `json:"dial_timeout_ms"`
	DropOnDisable          bool         `json:"drop_on_disable"`
	Worker                 WorkerConfig `json:"worker"`
}

func Default() Config {
	return Config{
		ListenAddr:             ":443",
		AdminSocket:            "/run/mtprotor/admin.sock",
		StateFile:              "/var/lib/mtprotor/secrets.json",
		LogLevel:               "info",
		HandshakeTimeoutMillis: 5000,
		DialTimeoutMillis:      5000,
		DropOnDisable:          false,
		Worker: WorkerConfig{
			Mode:                   "command",
			Command:                "/usr/local/bin/mtproto-proxy",
			Args:                   []string{"-H", "{{port}}", "-S", "{{secret}}"},
			Env:                    map[string]string{},
			PortRangeStart:         29000,
			PortRangeEnd:           29999,
			StartTimeoutMillis:     5000,
			StopGracePeriodSeconds: 10,
			BuiltinUpstreamAddr:    "",
		},
	}
}

func Load(path string) (Config, error) {
	cfg := Default()

	if path == "" {
		return cfg, nil
	}

	b, err := os.ReadFile(path)
	if err != nil {
		return Config{}, fmt.Errorf("read config: %w", err)
	}
	if err := json.Unmarshal(b, &cfg); err != nil {
		return Config{}, fmt.Errorf("parse config: %w", err)
	}
	if err := cfg.Validate(); err != nil {
		return Config{}, err
	}

	cfg.AdminSocket = filepath.Clean(cfg.AdminSocket)
	cfg.StateFile = filepath.Clean(cfg.StateFile)
	return cfg, nil
}

func (c Config) Validate() error {
	if c.ListenAddr == "" {
		return errors.New("listen_addr is required")
	}
	if c.AdminSocket == "" {
		return errors.New("admin_socket is required")
	}
	if c.StateFile == "" {
		return errors.New("state_file is required")
	}
	if c.HandshakeTimeoutMillis <= 0 {
		return errors.New("handshake_timeout_ms must be > 0")
	}
	if c.DialTimeoutMillis <= 0 {
		return errors.New("dial_timeout_ms must be > 0")
	}
	if c.Worker.PortRangeStart <= 0 || c.Worker.PortRangeEnd <= 0 || c.Worker.PortRangeEnd < c.Worker.PortRangeStart {
		return errors.New("invalid worker port range")
	}
	if c.Worker.StartTimeoutMillis <= 0 {
		return errors.New("worker.start_timeout_ms must be > 0")
	}
	if c.Worker.StopGracePeriodSeconds < 0 {
		return errors.New("worker.stop_grace_period_seconds must be >= 0")
	}
	if c.Worker.Mode != "command" && c.Worker.Mode != "builtin" {
		return errors.New("worker.mode must be command or builtin")
	}
	if c.Worker.Mode == "command" && c.Worker.Command == "" {
		return errors.New("worker.command is required for command mode")
	}
	if c.Worker.Mode == "builtin" && c.Worker.BuiltinUpstreamAddr == "" {
		return errors.New("worker.builtin_upstream_addr is required for builtin mode")
	}
	return nil
}
