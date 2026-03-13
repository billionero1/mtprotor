package storage

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"github.com/example/mtprotor/internal/model"
)

type StateStore struct {
	path string
}

func New(path string) *StateStore {
	return &StateStore{path: path}
}

func (s *StateStore) Load() (model.StateFile, error) {
	if _, err := os.Stat(s.path); errors.Is(err, os.ErrNotExist) {
		return model.StateFile{Version: 1, Secrets: []model.SecretRecord{}}, nil
	} else if err != nil {
		return model.StateFile{}, fmt.Errorf("stat state file: %w", err)
	}

	b, err := os.ReadFile(s.path)
	if err != nil {
		return model.StateFile{}, fmt.Errorf("read state file: %w", err)
	}

	var st model.StateFile
	if err := json.Unmarshal(b, &st); err != nil {
		return model.StateFile{}, fmt.Errorf("decode state file: %w", err)
	}
	if st.Version == 0 {
		st.Version = 1
	}
	if st.Secrets == nil {
		st.Secrets = []model.SecretRecord{}
	}
	return st, nil
}

func (s *StateStore) Save(st model.StateFile) error {
	dir := filepath.Dir(s.path)
	if err := os.MkdirAll(dir, 0o750); err != nil {
		return fmt.Errorf("mkdir for state file: %w", err)
	}

	b, err := json.MarshalIndent(st, "", "  ")
	if err != nil {
		return fmt.Errorf("encode state file: %w", err)
	}

	tmp, err := os.CreateTemp(dir, "secrets-*.tmp")
	if err != nil {
		return fmt.Errorf("create temp state file: %w", err)
	}
	tmpPath := tmp.Name()

	cleanup := func() {
		_ = tmp.Close()
		_ = os.Remove(tmpPath)
	}

	if _, err := tmp.Write(b); err != nil {
		cleanup()
		return fmt.Errorf("write temp state file: %w", err)
	}
	if err := tmp.Sync(); err != nil {
		cleanup()
		return fmt.Errorf("sync temp state file: %w", err)
	}
	if err := tmp.Chmod(0o600); err != nil {
		cleanup()
		return fmt.Errorf("chmod temp state file: %w", err)
	}
	if err := tmp.Close(); err != nil {
		cleanup()
		return fmt.Errorf("close temp state file: %w", err)
	}

	if err := os.Rename(tmpPath, s.path); err != nil {
		cleanup()
		return fmt.Errorf("rename temp state file: %w", err)
	}

	return nil
}
