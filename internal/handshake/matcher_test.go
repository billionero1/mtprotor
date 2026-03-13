package handshake

import "testing"

func TestNormalizeSecret(t *testing.T) {
	tests := []struct {
		name    string
		secret  string
		wantErr bool
	}{
		{name: "plain16", secret: "00112233445566778899aabbccddeeff"},
		{name: "with0x", secret: "0x00112233445566778899aabbccddeeff"},
		{name: "dd-prefixed", secret: "dd00112233445566778899aabbccddeeff"},
		{name: "invalid", secret: "xyz", wantErr: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, _, err := NormalizeSecret(tt.secret)
			if tt.wantErr && err == nil {
				t.Fatalf("expected error")
			}
			if !tt.wantErr && err != nil {
				t.Fatalf("unexpected error: %v", err)
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
