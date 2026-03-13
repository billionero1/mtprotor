package model

import "time"

// SecretRecord describes one managed MTProxy secret.
type SecretRecord struct {
	ID             string     `json:"id"`
	Secret         string     `json:"secret"`
	Label          string     `json:"label,omitempty"`
	Enabled        bool       `json:"enabled"`
	CreatedAt      time.Time  `json:"created_at"`
	UpdatedAt      time.Time  `json:"updated_at"`
	ExpiresAt      *time.Time `json:"expires_at,omitempty"`
	MaxConnections int        `json:"max_connections,omitempty"`
	TotalAccepted  uint64     `json:"total_accepted,omitempty"`
}

func (s SecretRecord) IsExpired(now time.Time) bool {
	if s.ExpiresAt == nil {
		return false
	}
	return now.After(*s.ExpiresAt)
}

type StateFile struct {
	Version int            `json:"version"`
	Secrets []SecretRecord `json:"secrets"`
}
