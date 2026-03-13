package handshake

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/example/mtprotor/internal/model"
)

const HandshakeSize = 64

var AllowedProtocolTags = map[uint32]struct{}{
	0xdddddddd: {}, // intermediate
	0xeeeeeeee: {}, // secure
	0xefefefef: {}, // abridged
}

type ParsedSecret struct {
	Normalized string
	Key        []byte
	Prefix     string
	TLSDomain  string
}

func ParseSecret(raw string) (ParsedSecret, error) {
	s := strings.TrimSpace(strings.ToLower(raw))
	s = strings.TrimPrefix(s, "0x")
	if len(s)%2 != 0 {
		return ParsedSecret{}, errors.New("secret hex length must be even")
	}

	decoded, err := hex.DecodeString(s)
	if err != nil {
		return ParsedSecret{}, fmt.Errorf("decode secret: %w", err)
	}

	switch {
	case len(decoded) == 16:
		key := append([]byte(nil), decoded...)
		return ParsedSecret{
			Normalized: hex.EncodeToString(key),
			Key:        key,
			Prefix:     "plain",
		}, nil
	case len(decoded) == 17 && decoded[0] == 0xdd:
		key := append([]byte(nil), decoded[1:17]...)
		return ParsedSecret{
			Normalized: hex.EncodeToString(key),
			Key:        key,
			Prefix:     "dd",
		}, nil
	case len(decoded) >= 17 && decoded[0] == 0xee:
		key := append([]byte(nil), decoded[1:17]...)
		domain, err := decodeTLSDomainSuffix(decoded[17:])
		if err != nil {
			return ParsedSecret{}, err
		}
		return ParsedSecret{
			Normalized: hex.EncodeToString(key),
			Key:        key,
			Prefix:     "ee",
			TLSDomain:  domain,
		}, nil
	default:
		return ParsedSecret{}, errors.New("unsupported secret format: expected 16-byte hex, dd+16-byte, or ee+16-byte(+optional suffix)")
	}
}

func NormalizeSecret(raw string) (string, []byte, error) {
	parsed, err := ParseSecret(raw)
	if err != nil {
		return "", nil, err
	}
	return parsed.Normalized, parsed.Key, nil
}

func MatchSecret(records []model.SecretRecord, first64 []byte, now time.Time) (model.SecretRecord, bool) {
	for _, rec := range records {
		if !rec.Enabled || rec.IsExpired(now) {
			continue
		}
		ok, err := Matches(rec.Secret, first64)
		if err != nil {
			continue
		}
		if ok {
			return rec, true
		}
	}
	return model.SecretRecord{}, false
}

func Matches(rawSecret string, first64 []byte) (bool, error) {
	if len(first64) < HandshakeSize {
		return false, errors.New("first64 has insufficient size")
	}

	_, secretKey, err := NormalizeSecret(rawSecret)
	if err != nil {
		return false, err
	}

	keyMaterial := first64[8:40]
	iv := first64[40:56]
	keyHash := sha256.Sum256(append(append([]byte{}, keyMaterial...), secretKey...))

	block, err := aes.NewCipher(keyHash[:])
	if err != nil {
		return false, fmt.Errorf("create aes cipher: %w", err)
	}

	decrypted := make([]byte, HandshakeSize)
	copy(decrypted, first64[:HandshakeSize])
	stream := cipher.NewCTR(block, iv)
	stream.XORKeyStream(decrypted, decrypted)

	tag := binary.LittleEndian.Uint32(decrypted[56:60])
	if _, ok := AllowedProtocolTags[tag]; !ok {
		return false, nil
	}
	dcID := int16(binary.LittleEndian.Uint16(decrypted[60:62]))
	if dcID == 0 || dcID > 1000 || dcID < -1000 {
		return false, nil
	}

	return true, nil
}

func BuildClientPreamble(rawSecret string, tag uint32, dcID int16) ([]byte, error) {
	_, secretKey, err := NormalizeSecret(rawSecret)
	if err != nil {
		return nil, err
	}

	buf := make([]byte, HandshakeSize)
	if _, err := rand.Read(buf); err != nil {
		return nil, fmt.Errorf("random preamble: %w", err)
	}

	if buf[0] == 0xef {
		buf[0] = 0x7f
	}
	for i := 0; i < 4; i++ {
		if buf[i] == 0 {
			buf[i] = byte(i + 1)
		}
	}
	if binary.LittleEndian.Uint32(buf[4:8]) == 0 {
		buf[4] = 1
	}

	plainTail := make([]byte, 8)
	binary.LittleEndian.PutUint32(plainTail[0:4], tag)
	binary.LittleEndian.PutUint16(plainTail[4:6], uint16(dcID))
	if _, err := rand.Read(plainTail[6:8]); err != nil {
		return nil, fmt.Errorf("random tail suffix: %w", err)
	}
	copy(buf[56:64], plainTail)

	keyMaterial := buf[8:40]
	iv := buf[40:56]
	keyHash := sha256.Sum256(append(append([]byte{}, keyMaterial...), secretKey...))

	block, err := aes.NewCipher(keyHash[:])
	if err != nil {
		return nil, fmt.Errorf("create aes cipher: %w", err)
	}
	// MTProxy alignment: consume first 56 bytes of CTR stream before payload tail.
	stream := cipher.NewCTR(block, iv)
	drop := make([]byte, 56)
	stream.XORKeyStream(drop, drop)
	stream.XORKeyStream(buf[56:64], buf[56:64])
	return buf, nil
}

func decodeTLSDomainSuffix(raw []byte) (string, error) {
	if len(raw) == 0 {
		return "", nil
	}
	domain, err := NormalizeTLSDomain(string(raw))
	if err != nil {
		return "", errors.New("invalid tls domain suffix")
	}
	return domain, nil
}

func NormalizeTLSDomain(domain string) (string, error) {
	domain = strings.TrimSpace(strings.ToLower(domain))
	if domain == "" {
		return "", nil
	}
	if len(domain) > 253 {
		return "", errors.New("tls domain is too long")
	}
	if strings.Contains(domain, "..") ||
		strings.HasPrefix(domain, ".") ||
		strings.HasSuffix(domain, ".") {
		return "", errors.New("invalid tls domain")
	}

	labels := strings.Split(domain, ".")
	for _, label := range labels {
		if label == "" || len(label) > 63 {
			return "", errors.New("invalid tls domain")
		}
		if strings.HasPrefix(label, "-") || strings.HasSuffix(label, "-") {
			return "", errors.New("invalid tls domain")
		}
		for i := 0; i < len(label); i++ {
			ch := label[i]
			isLetter := ch >= 'a' && ch <= 'z'
			isDigit := ch >= '0' && ch <= '9'
			if !isLetter && !isDigit && ch != '-' {
				return "", errors.New("invalid tls domain")
			}
		}
	}

	return domain, nil
}
