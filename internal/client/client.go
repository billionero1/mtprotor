package client

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"time"

	"github.com/example/mtprotor/internal/runtime"
)

type Client struct {
	socket string
	http   *http.Client
}

func New(socketPath string) *Client {
	transport := &http.Transport{
		DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
			var d net.Dialer
			return d.DialContext(ctx, "unix", socketPath)
		},
	}
	return &Client{
		socket: socketPath,
		http: &http.Client{
			Timeout:   8 * time.Second,
			Transport: transport,
		},
	}
}

func (c *Client) Status(ctx context.Context) (runtime.Status, error) {
	var out runtime.Status
	err := c.doJSON(ctx, http.MethodGet, "/v1/status", nil, &out)
	return out, err
}

func (c *Client) ListSecrets(ctx context.Context) ([]runtime.SecretView, error) {
	var out struct {
		Secrets []runtime.SecretView `json:"secrets"`
	}
	err := c.doJSON(ctx, http.MethodGet, "/v1/secrets", nil, &out)
	return out.Secrets, err
}

func (c *Client) AddSecret(ctx context.Context, in runtime.AddSecretInput) (runtime.SecretView, error) {
	body := map[string]any{
		"secret":          in.Secret,
		"label":           in.Label,
		"enabled":         in.Enabled,
		"max_connections": in.MaxConnections,
	}
	if in.ExpiresAt != nil {
		body["expires_at"] = in.ExpiresAt.UTC().Format(time.RFC3339)
	}

	var out runtime.SecretView
	err := c.doJSON(ctx, http.MethodPost, "/v1/secrets", body, &out)
	return out, err
}

func (c *Client) RemoveSecret(ctx context.Context, id string) error {
	return c.doJSON(ctx, http.MethodDelete, "/v1/secrets/"+id, nil, nil)
}

func (c *Client) EnableSecret(ctx context.Context, id string) (runtime.SecretView, error) {
	var out runtime.SecretView
	err := c.doJSON(ctx, http.MethodPatch, "/v1/secrets/"+id+"/enable", map[string]any{}, &out)
	return out, err
}

func (c *Client) DisableSecret(ctx context.Context, id string) (runtime.SecretView, error) {
	var out runtime.SecretView
	err := c.doJSON(ctx, http.MethodPatch, "/v1/secrets/"+id+"/disable", map[string]any{}, &out)
	return out, err
}

func (c *Client) doJSON(ctx context.Context, method, path string, reqBody any, out any) error {
	var bodyBytes []byte
	if reqBody != nil {
		b, err := json.Marshal(reqBody)
		if err != nil {
			return fmt.Errorf("encode request: %w", err)
		}
		bodyBytes = b
	}

	url := "http://unix" + path
	req, err := http.NewRequestWithContext(ctx, method, url, bytes.NewReader(bodyBytes))
	if err != nil {
		return fmt.Errorf("build request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.http.Do(req)
	if err != nil {
		return fmt.Errorf("request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 300 {
		var er struct {
			Error string `json:"error"`
		}
		_ = json.NewDecoder(resp.Body).Decode(&er)
		if er.Error == "" {
			er.Error = resp.Status
		}
		return fmt.Errorf("api error: %s", er.Error)
	}

	if out != nil {
		if err := json.NewDecoder(resp.Body).Decode(out); err != nil {
			return fmt.Errorf("decode response: %w", err)
		}
	}
	return nil
}
