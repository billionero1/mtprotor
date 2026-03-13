package api

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/example/mtprotor/internal/runtime"
)

type Controller interface {
	Status() runtime.Status
	ListSecrets() []runtime.SecretView
	AddSecret(in runtime.AddSecretInput) (runtime.SecretView, error)
	RemoveSecret(id string) error
	EnableSecret(id string) (runtime.SecretView, error)
	DisableSecret(id string) (runtime.SecretView, error)
}

type Server struct {
	socketPath string
	controller Controller
	logger     *slog.Logger

	ln     net.Listener
	httpS  *http.Server
	closed bool
}

func New(socketPath string, controller Controller, logger *slog.Logger) *Server {
	return &Server{
		socketPath: socketPath,
		controller: controller,
		logger:     logger,
	}
}

func (s *Server) Start() error {
	dir := filepath.Dir(s.socketPath)
	if err := os.MkdirAll(dir, 0o750); err != nil {
		return fmt.Errorf("mkdir socket dir: %w", err)
	}
	if err := removeIfExists(s.socketPath); err != nil {
		return err
	}

	ln, err := net.Listen("unix", s.socketPath)
	if err != nil {
		return fmt.Errorf("listen unix socket: %w", err)
	}
	if err := os.Chmod(s.socketPath, 0o660); err != nil {
		_ = ln.Close()
		return fmt.Errorf("chmod socket: %w", err)
	}
	s.ln = ln

	mux := http.NewServeMux()
	mux.HandleFunc("/v1/status", s.handleStatus)
	mux.HandleFunc("/v1/secrets", s.handleSecrets)
	mux.HandleFunc("/v1/secrets/", s.handleSecretAction)

	s.httpS = &http.Server{
		Handler:           loggingMiddleware(mux, s.logger),
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		if err := s.httpS.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			s.logger.Error("admin api serve failed", "err", err)
		}
	}()

	s.logger.Info("admin api started", "socket", s.socketPath)
	return nil
}

func (s *Server) Shutdown(ctx context.Context) error {
	if s.closed {
		return nil
	}
	s.closed = true

	var err error
	if s.httpS != nil {
		err = s.httpS.Shutdown(ctx)
	}
	if s.ln != nil {
		_ = s.ln.Close()
	}
	_ = os.Remove(s.socketPath)
	return err
}

func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	writeJSON(w, http.StatusOK, s.controller.Status())
}

func (s *Server) handleSecrets(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, http.StatusOK, map[string]any{"secrets": s.controller.ListSecrets()})
	case http.MethodPost:
		var req struct {
			Secret         string  `json:"secret"`
			Label          string  `json:"label"`
			Enabled        *bool   `json:"enabled"`
			ExpiresAt      *string `json:"expires_at"`
			MaxConnections int     `json:"max_connections"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeErr(w, http.StatusBadRequest, "invalid json")
			return
		}

		enabled := true
		if req.Enabled != nil {
			enabled = *req.Enabled
		}
		var expiresAt *time.Time
		if req.ExpiresAt != nil && strings.TrimSpace(*req.ExpiresAt) != "" {
			t, err := time.Parse(time.RFC3339, *req.ExpiresAt)
			if err != nil {
				writeErr(w, http.StatusBadRequest, "expires_at must be RFC3339")
				return
			}
			t = t.UTC()
			expiresAt = &t
		}

		item, err := s.controller.AddSecret(runtime.AddSecretInput{
			Secret:         req.Secret,
			Label:          req.Label,
			Enabled:        enabled,
			ExpiresAt:      expiresAt,
			MaxConnections: req.MaxConnections,
		})
		if err != nil {
			writeErr(w, http.StatusBadRequest, err.Error())
			return
		}
		writeJSON(w, http.StatusCreated, item)
	default:
		writeErr(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}

func (s *Server) handleSecretAction(w http.ResponseWriter, r *http.Request) {
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/v1/secrets/"), "/")
	if len(parts) == 0 || parts[0] == "" {
		writeErr(w, http.StatusNotFound, "not found")
		return
	}
	id := parts[0]

	if len(parts) == 1 {
		if r.Method != http.MethodDelete {
			writeErr(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		if err := s.controller.RemoveSecret(id); err != nil {
			writeErr(w, http.StatusNotFound, err.Error())
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})
		return
	}

	if len(parts) == 2 && r.Method == http.MethodPatch {
		switch parts[1] {
		case "enable":
			item, err := s.controller.EnableSecret(id)
			if err != nil {
				writeErr(w, http.StatusBadRequest, err.Error())
				return
			}
			writeJSON(w, http.StatusOK, item)
			return
		case "disable":
			item, err := s.controller.DisableSecret(id)
			if err != nil {
				writeErr(w, http.StatusBadRequest, err.Error())
				return
			}
			writeJSON(w, http.StatusOK, item)
			return
		}
	}

	writeErr(w, http.StatusNotFound, "not found")
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]any{"error": msg})
}

func loggingMiddleware(next http.Handler, logger *slog.Logger) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		logger.Debug("admin request", "method", r.Method, "path", r.URL.Path, "dur", time.Since(start))
	})
}

func removeIfExists(path string) error {
	fi, err := os.Stat(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("stat socket path: %w", err)
	}
	if fi.Mode()&os.ModeSocket == 0 {
		return fmt.Errorf("%s exists and is not a socket", path)
	}
	if err := os.Remove(path); err != nil {
		return fmt.Errorf("remove stale socket: %w", err)
	}
	return nil
}
