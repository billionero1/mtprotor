package runtime

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"sort"
	"sync"
	"time"

	"github.com/example/mtprotor/internal/config"
	"github.com/example/mtprotor/internal/handshake"
	"github.com/example/mtprotor/internal/model"
	"github.com/example/mtprotor/internal/storage"
	"github.com/example/mtprotor/internal/worker"
)

type AddSecretInput struct {
	Secret         string
	Label          string
	Enabled        bool
	ExpiresAt      *time.Time
	MaxConnections int
}

type SecretView struct {
	ID             string     `json:"id"`
	SecretMasked   string     `json:"secret_masked"`
	Label          string     `json:"label,omitempty"`
	Enabled        bool       `json:"enabled"`
	CreatedAt      time.Time  `json:"created_at"`
	UpdatedAt      time.Time  `json:"updated_at"`
	ExpiresAt      *time.Time `json:"expires_at,omitempty"`
	MaxConnections int        `json:"max_connections,omitempty"`
	ActiveConns    int        `json:"active_connections"`
	TotalAccepted  uint64     `json:"total_accepted"`
}

type Status struct {
	StartedAt      time.Time `json:"started_at"`
	UptimeSeconds  int64     `json:"uptime_seconds"`
	ListenAddr     string    `json:"listen_addr"`
	SecretsTotal   int       `json:"secrets_total"`
	SecretsEnabled int       `json:"secrets_enabled"`
	ConnsActive    int       `json:"connections_active"`
}

type Runtime struct {
	cfg       config.Config
	logger    *slog.Logger
	store     *storage.StateStore
	workers   *worker.Manager
	startedAt time.Time

	mu           sync.RWMutex
	secrets      map[string]model.SecretRecord
	activeByID   map[string]int
	drainingByID map[string]bool
	listener     net.Listener
	acceptWG     sync.WaitGroup
	shutdownOnce sync.Once
	shutdownCh   chan struct{}
	shutdownErr  error
	persistMu    sync.Mutex
}

func New(cfg config.Config, logger *slog.Logger) *Runtime {
	return &Runtime{
		cfg:          cfg,
		logger:       logger,
		store:        storage.New(cfg.StateFile),
		workers:      worker.NewManager(cfg.Worker, logger),
		secrets:      map[string]model.SecretRecord{},
		activeByID:   map[string]int{},
		drainingByID: map[string]bool{},
		shutdownCh:   make(chan struct{}),
	}
}

func (r *Runtime) Start() error {
	if err := r.loadState(); err != nil {
		return err
	}

	ln, err := net.Listen("tcp", r.cfg.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", r.cfg.ListenAddr, err)
	}
	r.listener = ln
	r.startedAt = time.Now().UTC()

	r.acceptWG.Add(1)
	go r.acceptLoop()
	r.logger.Info("proxy listener started", "addr", ln.Addr().String())
	return nil
}

func (r *Runtime) Shutdown(ctx context.Context) error {
	r.shutdownOnce.Do(func() {
		close(r.shutdownCh)
		if r.listener != nil {
			r.shutdownErr = r.listener.Close()
		}
	})

	done := make(chan struct{})
	go func() {
		r.acceptWG.Wait()
		close(done)
	}()

	select {
	case <-done:
	case <-ctx.Done():
		return ctx.Err()
	}

	r.workers.StopAll()
	return r.shutdownErr
}

func (r *Runtime) Status() Status {
	r.mu.RLock()
	defer r.mu.RUnlock()

	enabled := 0
	active := 0
	for _, s := range r.secrets {
		if s.Enabled && !s.IsExpired(time.Now().UTC()) {
			enabled++
		}
	}
	for _, c := range r.activeByID {
		active += c
	}

	uptime := int64(0)
	if !r.startedAt.IsZero() {
		uptime = int64(time.Since(r.startedAt).Seconds())
	}

	return Status{
		StartedAt:      r.startedAt,
		UptimeSeconds:  uptime,
		ListenAddr:     r.listenAddr(),
		SecretsTotal:   len(r.secrets),
		SecretsEnabled: enabled,
		ConnsActive:    active,
	}
}

func (r *Runtime) ProxyAddr() string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.listenAddr()
}

func (r *Runtime) ListSecrets() []SecretView {
	r.mu.RLock()
	defer r.mu.RUnlock()

	items := make([]SecretView, 0, len(r.secrets))
	for _, rec := range r.secrets {
		items = append(items, SecretView{
			ID:             rec.ID,
			SecretMasked:   maskSecret(rec.Secret),
			Label:          rec.Label,
			Enabled:        rec.Enabled,
			CreatedAt:      rec.CreatedAt,
			UpdatedAt:      rec.UpdatedAt,
			ExpiresAt:      rec.ExpiresAt,
			MaxConnections: rec.MaxConnections,
			ActiveConns:    r.activeByID[rec.ID],
			TotalAccepted:  rec.TotalAccepted,
		})
	}
	sort.Slice(items, func(i, j int) bool {
		return items[i].CreatedAt.Before(items[j].CreatedAt)
	})
	return items
}

func (r *Runtime) AddSecret(in AddSecretInput) (SecretView, error) {
	normalized, _, err := handshake.NormalizeSecret(in.Secret)
	if err != nil {
		return SecretView{}, err
	}
	if in.MaxConnections < 0 {
		return SecretView{}, errors.New("max_connections must be >= 0")
	}

	now := time.Now().UTC()
	id := secretID(normalized)
	if in.ExpiresAt != nil && in.ExpiresAt.Before(now) {
		return SecretView{}, errors.New("expires_at is in the past")
	}

	r.mu.Lock()
	if existing, ok := r.secrets[id]; ok {
		r.mu.Unlock()
		return SecretView{}, fmt.Errorf("secret already exists: %s", existing.ID)
	}

	rec := model.SecretRecord{
		ID:             id,
		Secret:         normalized,
		Label:          in.Label,
		Enabled:        in.Enabled,
		CreatedAt:      now,
		UpdatedAt:      now,
		ExpiresAt:      in.ExpiresAt,
		MaxConnections: in.MaxConnections,
	}
	r.secrets[id] = rec
	delete(r.drainingByID, id)
	err = r.persistLocked()
	r.mu.Unlock()
	if err != nil {
		return SecretView{}, err
	}

	if rec.Enabled && !rec.IsExpired(now) {
		if _, err := r.workers.Start(rec.ID, rec.Secret); err != nil {
			r.mu.Lock()
			delete(r.secrets, rec.ID)
			_ = r.persistLocked()
			r.mu.Unlock()
			return SecretView{}, fmt.Errorf("start worker for secret: %w", err)
		}
	}

	r.logger.Info("secret added", "secret_id", rec.ID, "enabled", rec.Enabled)
	return r.getSecretView(rec.ID), nil
}

func (r *Runtime) RemoveSecret(id string) error {
	r.mu.Lock()
	rec, ok := r.secrets[id]
	if !ok {
		r.mu.Unlock()
		return fmt.Errorf("secret not found: %s", id)
	}
	delete(r.secrets, id)
	err := r.persistLocked()
	active := r.activeByID[id]
	if active > 0 && !r.cfg.DropOnDisable {
		r.drainingByID[id] = true
	}
	r.mu.Unlock()
	if err != nil {
		return err
	}

	if active == 0 || r.cfg.DropOnDisable {
		if stopErr := r.workers.Stop(id); stopErr != nil {
			r.logger.Error("worker stop after remove failed", "secret_id", id, "err", stopErr)
		}
	}

	r.logger.Info("secret removed", "secret_id", rec.ID, "active_connections", active)
	return nil
}

func (r *Runtime) EnableSecret(id string) (SecretView, error) {
	r.mu.Lock()
	rec, ok := r.secrets[id]
	if !ok {
		r.mu.Unlock()
		return SecretView{}, fmt.Errorf("secret not found: %s", id)
	}
	if rec.IsExpired(time.Now().UTC()) {
		r.mu.Unlock()
		return SecretView{}, errors.New("cannot enable expired secret")
	}
	rec.Enabled = true
	rec.UpdatedAt = time.Now().UTC()
	r.secrets[id] = rec
	err := r.persistLocked()
	delete(r.drainingByID, id)
	r.mu.Unlock()
	if err != nil {
		return SecretView{}, err
	}
	if _, err := r.workers.Start(rec.ID, rec.Secret); err != nil {
		return SecretView{}, err
	}
	r.logger.Info("secret enabled", "secret_id", id)
	return r.getSecretView(id), nil
}

func (r *Runtime) DisableSecret(id string) (SecretView, error) {
	r.mu.Lock()
	rec, ok := r.secrets[id]
	if !ok {
		r.mu.Unlock()
		return SecretView{}, fmt.Errorf("secret not found: %s", id)
	}
	rec.Enabled = false
	rec.UpdatedAt = time.Now().UTC()
	r.secrets[id] = rec
	active := r.activeByID[id]
	err := r.persistLocked()
	if active > 0 && !r.cfg.DropOnDisable {
		r.drainingByID[id] = true
	}
	r.mu.Unlock()
	if err != nil {
		return SecretView{}, err
	}

	if active == 0 || r.cfg.DropOnDisable {
		_ = r.workers.Stop(id)
	}
	return r.getSecretView(id), nil
}

func (r *Runtime) acceptLoop() {
	defer r.acceptWG.Done()
	for {
		conn, err := r.listener.Accept()
		if err != nil {
			select {
			case <-r.shutdownCh:
				return
			default:
				r.logger.Error("accept failed", "err", err)
				time.Sleep(100 * time.Millisecond)
				continue
			}
		}
		r.acceptWG.Add(1)
		go func() {
			defer r.acceptWG.Done()
			r.handleConn(conn)
		}()
	}
}

func (r *Runtime) handleConn(client net.Conn) {
	defer client.Close()
	_ = client.SetReadDeadline(time.Now().Add(time.Duration(r.cfg.HandshakeTimeoutMillis) * time.Millisecond))
	first := make([]byte, handshake.HandshakeSize)
	if _, err := io.ReadFull(client, first); err != nil {
		r.logger.Debug("handshake read failed", "err", err)
		return
	}
	_ = client.SetReadDeadline(time.Time{})

	records := r.enabledSecretsSnapshot(time.Now().UTC())
	rec, ok := handshake.MatchSecret(records, first, time.Now().UTC())
	if !ok {
		r.logger.Debug("secret mismatch; drop connection", "remote", client.RemoteAddr().String())
		return
	}

	if rec.MaxConnections > 0 {
		r.mu.RLock()
		active := r.activeByID[rec.ID]
		r.mu.RUnlock()
		if active >= rec.MaxConnections {
			r.logger.Info("connection rejected by limit", "secret_id", rec.ID, "active", active, "limit", rec.MaxConnections)
			return
		}
	}

	addr, ok := r.workers.Addr(rec.ID)
	if !ok {
		inst, err := r.workers.Start(rec.ID, rec.Secret)
		if err != nil {
			r.logger.Error("worker start failed", "secret_id", rec.ID, "err", err)
			return
		}
		addr = inst.Addr
	}

	backend, err := net.DialTimeout("tcp", addr, time.Duration(r.cfg.DialTimeoutMillis)*time.Millisecond)
	if err != nil {
		r.logger.Error("dial worker failed", "secret_id", rec.ID, "worker_addr", addr, "err", err)
		return
	}
	defer backend.Close()

	if _, err := backend.Write(first); err != nil {
		r.logger.Error("write preamble to worker failed", "secret_id", rec.ID, "err", err)
		return
	}

	r.incConn(rec.ID)
	r.incAccepted(rec.ID)
	defer r.decConn(rec.ID)

	errC := make(chan error, 2)
	go func() {
		_, err := io.Copy(backend, client)
		errC <- err
	}()
	go func() {
		_, err := io.Copy(client, backend)
		errC <- err
	}()
	<-errC
}

func (r *Runtime) loadState() error {
	state, err := r.store.Load()
	if err != nil {
		return err
	}

	now := time.Now().UTC()
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, rec := range state.Secrets {
		r.secrets[rec.ID] = rec
		if rec.Enabled && !rec.IsExpired(now) {
			if _, err := r.workers.Start(rec.ID, rec.Secret); err != nil {
				r.logger.Error("worker restore failed", "secret_id", rec.ID, "err", err)
			}
		}
	}
	return nil
}

func (r *Runtime) enabledSecretsSnapshot(now time.Time) []model.SecretRecord {
	r.mu.RLock()
	defer r.mu.RUnlock()
	list := make([]model.SecretRecord, 0, len(r.secrets))
	for _, rec := range r.secrets {
		if rec.Enabled && !rec.IsExpired(now) {
			list = append(list, rec)
		}
	}
	return list
}

func (r *Runtime) incConn(secretID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.activeByID[secretID]++
}

func (r *Runtime) decConn(secretID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if v := r.activeByID[secretID]; v > 0 {
		r.activeByID[secretID] = v - 1
	}
	if r.activeByID[secretID] == 0 && r.drainingByID[secretID] {
		delete(r.drainingByID, secretID)
		go func(id string) {
			_ = r.workers.Stop(id)
		}(secretID)
	}
}

func (r *Runtime) incAccepted(secretID string) {
	r.mu.Lock()
	rec, ok := r.secrets[secretID]
	if ok {
		rec.TotalAccepted++
		r.secrets[secretID] = rec
	}
	r.mu.Unlock()
}

func (r *Runtime) getSecretView(id string) SecretView {
	r.mu.RLock()
	defer r.mu.RUnlock()
	rec := r.secrets[id]
	return SecretView{
		ID:             rec.ID,
		SecretMasked:   maskSecret(rec.Secret),
		Label:          rec.Label,
		Enabled:        rec.Enabled,
		CreatedAt:      rec.CreatedAt,
		UpdatedAt:      rec.UpdatedAt,
		ExpiresAt:      rec.ExpiresAt,
		MaxConnections: rec.MaxConnections,
		ActiveConns:    r.activeByID[id],
		TotalAccepted:  rec.TotalAccepted,
	}
}

func (r *Runtime) persistLocked() error {
	r.persistMu.Lock()
	defer r.persistMu.Unlock()

	items := make([]model.SecretRecord, 0, len(r.secrets))
	for _, rec := range r.secrets {
		items = append(items, rec)
	}
	sort.Slice(items, func(i, j int) bool {
		return items[i].CreatedAt.Before(items[j].CreatedAt)
	})
	return r.store.Save(model.StateFile{Version: 1, Secrets: items})
}

func (r *Runtime) listenAddr() string {
	if r.listener != nil {
		return r.listener.Addr().String()
	}
	return r.cfg.ListenAddr
}

func secretID(normalizedSecret string) string {
	sum := sha256.Sum256([]byte(normalizedSecret))
	return hex.EncodeToString(sum[:16])
}

func maskSecret(s string) string {
	if len(s) <= 8 {
		return "****"
	}
	return s[:4] + "..." + s[len(s)-4:]
}
