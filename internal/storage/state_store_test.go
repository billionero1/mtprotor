package storage

import (
	"path/filepath"
	"testing"
	"time"

	"github.com/example/mtprotor/internal/model"
)

func TestStateStoreSaveLoad(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	store := New(filepath.Join(dir, "secrets.json"))

	now := time.Now().UTC().Truncate(time.Second)
	state := model.StateFile{
		Version: 1,
		Secrets: []model.SecretRecord{{
			ID:        "id1",
			Secret:    "00112233445566778899aabbccddeeff",
			Enabled:   true,
			CreatedAt: now,
			UpdatedAt: now,
		}},
	}

	if err := store.Save(state); err != nil {
		t.Fatalf("save: %v", err)
	}

	loaded, err := store.Load()
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if loaded.Version != 1 || len(loaded.Secrets) != 1 {
		t.Fatalf("unexpected loaded data: %+v", loaded)
	}
	if loaded.Secrets[0].ID != "id1" {
		t.Fatalf("unexpected secret id: %s", loaded.Secrets[0].ID)
	}
}
