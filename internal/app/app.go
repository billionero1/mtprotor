package app

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/example/mtprotor/internal/api"
	"github.com/example/mtprotor/internal/client"
	"github.com/example/mtprotor/internal/config"
	"github.com/example/mtprotor/internal/runtime"
)

func Run(args []string) int {
	if len(args) == 0 {
		printUsage()
		return 1
	}

	switch args[0] {
	case "daemon", "start":
		return runDaemon(args[1:])
	case "status", "health":
		return runStatus(args[1:])
	case "secret":
		return runSecret(args[1:])
	case "help", "-h", "--help":
		printUsage()
		return 0
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n", args[0])
		printUsage()
		return 1
	}
}

func runDaemon(args []string) int {
	fs := flag.NewFlagSet("daemon", flag.ContinueOnError)
	configPath := fs.String("config", "/etc/mtprotor/config.json", "Path to config file")
	if err := fs.Parse(args); err != nil {
		return 1
	}

	cfg, err := config.Load(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "config error: %v\n", err)
		return 1
	}

	logger := newLogger(cfg.LogLevel)
	rt := runtime.New(cfg, logger)
	if err := rt.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "runtime start failed: %v\n", err)
		return 1
	}

	apiServer := api.New(cfg.AdminSocket, rt, logger)
	if err := apiServer.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "admin api start failed: %v\n", err)
		_ = rt.Shutdown(context.Background())
		return 1
	}

	sigC := make(chan os.Signal, 2)
	signal.Notify(sigC, syscall.SIGINT, syscall.SIGTERM)
	sig := <-sigC
	logger.Info("shutdown signal received", "signal", sig.String())

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	_ = apiServer.Shutdown(ctx)
	if err := rt.Shutdown(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "shutdown failed: %v\n", err)
		return 1
	}
	return 0
}

func runStatus(args []string) int {
	fs := flag.NewFlagSet("status", flag.ContinueOnError)
	socket := fs.String("socket", "", "Path to admin unix socket")
	configPath := fs.String("config", "/etc/mtprotor/config.json", "Path to config file")
	if err := fs.Parse(args); err != nil {
		return 1
	}

	sock, err := resolveSocket(*socket, *configPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}

	cli := client.New(sock)
	st, err := cli.Status(context.Background())
	if err != nil {
		fmt.Fprintf(os.Stderr, "status failed: %v\n", err)
		return 1
	}
	return printJSON(st)
}

func runSecret(args []string) int {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "usage: mtprotor secret <list|add|remove|enable|disable> ...")
		return 1
	}

	sub := args[0]
	subArgs := args[1:]
	switch sub {
	case "list":
		return runSecretList(subArgs)
	case "add":
		return runSecretAdd(subArgs)
	case "remove":
		return runSecretRemove(subArgs)
	case "enable":
		return runSecretEnableDisable(subArgs, true)
	case "disable":
		return runSecretEnableDisable(subArgs, false)
	default:
		fmt.Fprintf(os.Stderr, "unknown secret subcommand %q\n", sub)
		return 1
	}
}

func runSecretList(args []string) int {
	fs := flag.NewFlagSet("secret list", flag.ContinueOnError)
	socket := fs.String("socket", "", "Path to admin unix socket")
	configPath := fs.String("config", "/etc/mtprotor/config.json", "Path to config file")
	if err := fs.Parse(args); err != nil {
		return 1
	}

	sock, err := resolveSocket(*socket, *configPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	cli := client.New(sock)
	items, err := cli.ListSecrets(context.Background())
	if err != nil {
		fmt.Fprintf(os.Stderr, "list failed: %v\n", err)
		return 1
	}
	return printJSON(map[string]any{"secrets": items})
}

func runSecretAdd(args []string) int {
	fs := flag.NewFlagSet("secret add", flag.ContinueOnError)
	label := fs.String("label", "", "Secret label")
	disabled := fs.Bool("disabled", false, "Create secret in disabled state")
	expiresAt := fs.String("expires-at", "", "RFC3339 UTC expiration")
	maxConn := fs.Int("max-connections", 0, "Max concurrent connections (0 = unlimited)")
	socket := fs.String("socket", "", "Path to admin unix socket")
	configPath := fs.String("config", "/etc/mtprotor/config.json", "Path to config file")
	if err := fs.Parse(args); err != nil {
		return 1
	}
	if fs.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: mtprotor secret add <secret_hex> [--label ...]")
		return 1
	}

	sock, err := resolveSocket(*socket, *configPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}

	var exp *time.Time
	if strings.TrimSpace(*expiresAt) != "" {
		t, err := time.Parse(time.RFC3339, *expiresAt)
		if err != nil {
			fmt.Fprintf(os.Stderr, "invalid --expires-at: %v\n", err)
			return 1
		}
		t = t.UTC()
		exp = &t
	}

	cli := client.New(sock)
	item, err := cli.AddSecret(context.Background(), runtime.AddSecretInput{
		Secret:         fs.Arg(0),
		Label:          *label,
		Enabled:        !*disabled,
		ExpiresAt:      exp,
		MaxConnections: *maxConn,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "add failed: %v\n", err)
		return 1
	}
	return printJSON(item)
}

func runSecretRemove(args []string) int {
	fs := flag.NewFlagSet("secret remove", flag.ContinueOnError)
	socket := fs.String("socket", "", "Path to admin unix socket")
	configPath := fs.String("config", "/etc/mtprotor/config.json", "Path to config file")
	if err := fs.Parse(args); err != nil {
		return 1
	}
	if fs.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: mtprotor secret remove <id>")
		return 1
	}

	sock, err := resolveSocket(*socket, *configPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}

	cli := client.New(sock)
	if err := cli.RemoveSecret(context.Background(), fs.Arg(0)); err != nil {
		fmt.Fprintf(os.Stderr, "remove failed: %v\n", err)
		return 1
	}
	return printJSON(map[string]any{"ok": true})
}

func runSecretEnableDisable(args []string, enable bool) int {
	action := "enable"
	if !enable {
		action = "disable"
	}
	fs := flag.NewFlagSet("secret "+action, flag.ContinueOnError)
	socket := fs.String("socket", "", "Path to admin unix socket")
	configPath := fs.String("config", "/etc/mtprotor/config.json", "Path to config file")
	if err := fs.Parse(args); err != nil {
		return 1
	}
	if fs.NArg() != 1 {
		fmt.Fprintf(os.Stderr, "usage: mtprotor secret %s <id>\n", action)
		return 1
	}
	sock, err := resolveSocket(*socket, *configPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	cli := client.New(sock)
	var item runtime.SecretView
	if enable {
		item, err = cli.EnableSecret(context.Background(), fs.Arg(0))
	} else {
		item, err = cli.DisableSecret(context.Background(), fs.Arg(0))
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s failed: %v\n", action, err)
		return 1
	}
	return printJSON(item)
}

func resolveSocket(socketOverride, cfgPath string) (string, error) {
	if socketOverride != "" {
		return socketOverride, nil
	}
	cfg, err := config.Load(cfgPath)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return config.Default().AdminSocket, nil
		}
		return "", fmt.Errorf("resolve socket from config: %w", err)
	}
	return cfg.AdminSocket, nil
}

func newLogger(level string) *slog.Logger {
	var lv slog.Level
	switch strings.ToLower(level) {
	case "debug":
		lv = slog.LevelDebug
	case "warn", "warning":
		lv = slog.LevelWarn
	case "error":
		lv = slog.LevelError
	default:
		lv = slog.LevelInfo
	}
	h := slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: lv})
	return slog.New(h)
}

func printJSON(v any) int {
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	if err := enc.Encode(v); err != nil {
		fmt.Fprintf(os.Stderr, "json encode failed: %v\n", err)
		return 1
	}
	return 0
}

func printUsage() {
	fmt.Print(`mtprotor - hot-reload runtime for MTProxy secrets

Usage:
  mtprotor daemon [--config /etc/mtprotor/config.json]
  mtprotor start  [--config /etc/mtprotor/config.json]
  mtprotor status [--config ... | --socket ...]
  mtprotor health [--config ... | --socket ...]

  mtprotor secret list [--config ... | --socket ...]
  mtprotor secret add <secret_hex> [--label main] [--disabled] [--expires-at RFC3339] [--max-connections N]
  mtprotor secret remove <id>
  mtprotor secret enable <id>
  mtprotor secret disable <id>
`)
}
