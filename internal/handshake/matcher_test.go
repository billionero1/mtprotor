package handshake

import "testing"

func TestNormalizeSecret(t *testing.T) {
	tests := []struct {
		name    string
		secret  string
		want    string
		wantErr bool
	}{
		{name: "plain16", secret: "00112233445566778899aabbccddeeff", want: "00112233445566778899aabbccddeeff"},
		{name: "with0x", secret: "0x00112233445566778899aabbccddeeff", want: "00112233445566778899aabbccddeeff"},
		{name: "dd-prefixed", secret: "dd00112233445566778899aabbccddeeff", want: "00112233445566778899aabbccddeeff"},
		{name: "ee-with-suffix", secret: "ee00112233445566778899aabbccddeeff6578616d706c652e636f6d", want: "00112233445566778899aabbccddeeff"},
		{name: "invalid", secret: "xyz", wantErr: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, _, err := NormalizeSecret(tt.secret)
			if tt.wantErr && err == nil {
				t.Fatalf("expected error")
			}
			if !tt.wantErr && err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if !tt.wantErr && got != tt.want {
				t.Fatalf("unexpected normalized secret: got=%s want=%s", got, tt.want)
			}
		})
	}
}

func TestParseSecret(t *testing.T) {
	tests := []struct {
		name       string
		secret     string
		wantNorm   string
		wantMode   string
		wantDomain string
		wantErr    bool
	}{
		{
			name:       "plain",
			secret:     "00112233445566778899aabbccddeeff",
			wantNorm:   "00112233445566778899aabbccddeeff",
			wantMode:   "plain",
			wantDomain: "",
		},
		{
			name:       "dd",
			secret:     "dd00112233445566778899aabbccddeeff",
			wantNorm:   "00112233445566778899aabbccddeeff",
			wantMode:   "dd",
			wantDomain: "",
		},
		{
			name:       "ee with domain",
			secret:     "ee00112233445566778899aabbccddeeff6578616d706c652e636f6d",
			wantNorm:   "00112233445566778899aabbccddeeff",
			wantMode:   "ee",
			wantDomain: "example.com",
		},
		{
			name:    "ee invalid domain",
			secret:  "ee00112233445566778899aabbccddeeff6578616d706c652e636f6d2f74657374",
			wantErr: true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := ParseSecret(tt.secret)
			if tt.wantErr {
				if err == nil {
					t.Fatalf("expected parse error")
				}
				return
			}
			if err != nil {
				t.Fatalf("parse secret: %v", err)
			}
			if got.Normalized != tt.wantNorm {
				t.Fatalf("unexpected normalized: got=%s want=%s", got.Normalized, tt.wantNorm)
			}
			if got.Prefix != tt.wantMode {
				t.Fatalf("unexpected mode: got=%s want=%s", got.Prefix, tt.wantMode)
			}
			if got.TLSDomain != tt.wantDomain {
				t.Fatalf("unexpected tls domain: got=%s want=%s", got.TLSDomain, tt.wantDomain)
			}
		})
	}
}

func TestNormalizeTLSDomain(t *testing.T) {
	tests := []struct {
		name    string
		in      string
		want    string
		wantErr bool
	}{
		{name: "empty", in: "", want: ""},
		{name: "trim lower", in: " ExAmPle.CoM ", want: "example.com"},
		{name: "invalid slash", in: "example.com/test", wantErr: true},
		{name: "invalid dash", in: "-example.com", wantErr: true},
		{name: "invalid double dot", in: "example..com", wantErr: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := NormalizeTLSDomain(tt.in)
			if tt.wantErr {
				if err == nil {
					t.Fatalf("expected error")
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if got != tt.want {
				t.Fatalf("unexpected normalized domain: got=%q want=%q", got, tt.want)
			}
		})
	}
}

func TestMatchRoundTrip(t *testing.T) {
	secret := "00112233445566778899aabbccddeeff"
	preamble, err := BuildClientPreamble(secret, 0xdddddddd, 2)
	if err != nil {
		t.Fatalf("build preamble: %v", err)
	}

	ok, err := Matches(secret, preamble)
	if err != nil {
		t.Fatalf("matches error: %v", err)
	}
	if !ok {
		t.Fatalf("expected match")
	}

	ok, err = Matches("ffeeddccbbaa99887766554433221100", preamble)
	if err != nil {
		t.Fatalf("matches error: %v", err)
	}
	if ok {
		t.Fatalf("expected mismatch")
	}
}
