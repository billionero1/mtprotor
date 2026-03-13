/*
    This file is part of Mtproto-proxy Library.

    Mtproto-proxy Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Mtproto-proxy Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with Mtproto-proxy Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2010-2013 Vkontakte Ltd
              2010-2013 Nikolai Durov
              2010-2013 Andrey Lopatin
                   2013 Vitaliy Valtman
    
    Copyright 2014-2018 Telegram Messenger Inc                 
              2015-2016 Vitaly Valtman
                    2016-2018 Nikolai Durov
*/

#define        _FILE_OFFSET_BITS        64

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/rand.h>

#include "common/kprintf.h"
#include "common/precise-time.h"
#include "common/resolver.h"
#include "common/rpc-const.h"
#include "common/sha256.h"
#include "net/net-connections.h"
#include "net/net-crypto-aes.h"
#include "net/net-events.h"
#include "net/net-tcp-connections.h"
#include "net/net-tcp-rpc-ext-server.h"
#include "net/net-thread.h"

#include "vv/vv-io.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/*
 *
 *                EXTERNAL RPC SERVER INTERFACE
 *
 */

int tcp_rpcs_compact_parse_execute (connection_job_t c);
int tcp_rpcs_ext_alarm (connection_job_t c);
int tcp_rpcs_ext_init_accepted (connection_job_t c);

conn_type_t ct_tcp_rpc_ext_server = {
  .magic = CONN_FUNC_MAGIC,
  .flags = C_RAWMSG,
  .title = "rpc_ext_server",
  .init_accepted = tcp_rpcs_ext_init_accepted,
  .parse_execute = tcp_rpcs_compact_parse_execute,
  .close = tcp_rpcs_close_connection,
  .flush = tcp_rpc_flush,
  .write_packet = tcp_rpc_write_packet_compact,
  .connected = server_failed,
  .wakeup = tcp_rpcs_wakeup,
  .alarm = tcp_rpcs_ext_alarm,
  .crypto_init = aes_crypto_ctr128_init,
  .crypto_free = aes_crypto_free,
  .crypto_encrypt_output = cpu_tcp_aes_crypto_ctr128_encrypt_output,
  .crypto_decrypt_input = cpu_tcp_aes_crypto_ctr128_decrypt_input,
  .crypto_needed_output_bytes = cpu_tcp_aes_crypto_ctr128_needed_output_bytes,
};

int tcp_proxy_pass_parse_execute (connection_job_t C);
int tcp_proxy_pass_close (connection_job_t C, int who);
int tcp_proxy_pass_connected (connection_job_t C);
int tcp_proxy_pass_write_packet (connection_job_t c, struct raw_message *raw); 

conn_type_t ct_proxy_pass = {
  .magic = CONN_FUNC_MAGIC,
  .flags = C_RAWMSG,
  .title = "proxypass",
  .init_accepted = server_failed,
  .parse_execute = tcp_proxy_pass_parse_execute,
  .connected = tcp_proxy_pass_connected,
  .close = tcp_proxy_pass_close,
  .write_packet = tcp_proxy_pass_write_packet,
  .connected = server_noop,
};

int tcp_proxy_pass_connected (connection_job_t C) {
  struct connection_info *c = CONN_INFO(C);
  vkprintf (1, "proxy pass connected #%d %s:%d -> %s:%d\n", c->fd, show_our_ip (C), c->our_port, show_remote_ip (C), c->remote_port);
  return 0;
}

int tcp_proxy_pass_parse_execute (connection_job_t C) {
  struct connection_info *c = CONN_INFO(C);
  if (!c->extra) {
    fail_connection (C, -1);
    return 0;
  }
  job_t E = job_incref (c->extra);
  struct connection_info *e = CONN_INFO(E);

  struct raw_message *r = malloc (sizeof (*r));
  rwm_move (r, &c->in);
  rwm_init (&c->in, 0);
  vkprintf (3, "proxying %d bytes to %s:%d\n", r->total_bytes, show_remote_ip (E), e->remote_port);
  mpq_push_w (e->out_queue, PTR_MOVE(r), 0);
  job_signal (JOB_REF_PASS (E), JS_RUN);
  return 0;
}

int tcp_proxy_pass_close (connection_job_t C, int who) {
  struct connection_info *c = CONN_INFO(C);
  vkprintf (1, "closing proxy pass connection #%d %s:%d -> %s:%d\n", c->fd, show_our_ip (C), c->our_port, show_remote_ip (C), c->remote_port);
  if (c->extra) {
    job_t E = PTR_MOVE (c->extra);
    fail_connection (E, -23);
    job_decref (JOB_REF_PASS (E));
  }
  return cpu_server_close_connection (C, who);
}

int tcp_proxy_pass_write_packet (connection_job_t C, struct raw_message *raw) {
  rwm_union (&CONN_INFO(C)->out, raw);
  return 0;
}

int tcp_rpcs_default_execute (connection_job_t c, int op, struct raw_message *msg);

#define EXT_SECRET_LABEL_MAX 96
#define EXT_SECRET_ADMIN_TOKEN_MAX 128

struct ext_secret_entry {
  unsigned char secret[16];
  char hex[33];
  char label[EXT_SECRET_LABEL_MAX];
  int enabled;
  long long created_at;
  long long updated_at;
  long long expires_at;
};

static pthread_rwlock_t ext_secret_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct ext_secret_entry *ext_secrets;
static int ext_secret_cnt;
static int ext_secret_cap;

static char ext_secret_state_file[PATH_MAX];
static char ext_secret_admin_socket[PATH_MAX];
static char ext_secret_admin_token[EXT_SECRET_ADMIN_TOKEN_MAX];
static pthread_t ext_secret_admin_thread;
static int ext_secret_admin_started;

static long long ext_now_sec (void) {
  return (long long) time (NULL);
}

static void ext_secret16_to_hex (const unsigned char secret[16], char hex[33]) {
  static const char xd[] = "0123456789abcdef";
  int i;
  for (i = 0; i < 16; i++) {
    hex[2 * i] = xd[secret[i] >> 4];
    hex[2 * i + 1] = xd[secret[i] & 0x0f];
  }
  hex[32] = 0;
}

static int ext_hex16_to_secret (const char *hex, unsigned char out[16]) {
  int i;
  for (i = 0; i < 32; i++) {
    if (!isxdigit ((unsigned char) hex[i])) {
      return -1;
    }
  }
  for (i = 0; i < 16; i++) {
    int hi = hex[2 * i];
    int lo = hex[2 * i + 1];
    hi = (hi <= '9') ? hi - '0' : (tolower (hi) - 'a' + 10);
    lo = (lo <= '9') ? lo - '0' : (tolower (lo) - 'a' + 10);
    out[i] = (unsigned char) ((hi << 4) | lo);
  }
  return 0;
}

static int ext_secret16_from_hex (const char *hex, unsigned char out[16]) {
  size_t n, i;
  if (!hex) {
    return -1;
  }
  n = strlen (hex);

  if (n == 32) {
    return ext_hex16_to_secret (hex, out);
  }

  // dd + 16-byte secret: random-padding mode link secret.
  if (n == 34 && !strncasecmp (hex, "dd", 2)) {
    return ext_hex16_to_secret (hex + 2, out);
  }

  // ee + 16-byte secret (+ optional hex suffix): TLS-like mode link secret.
  if (n >= 34 && !strncasecmp (hex, "ee", 2)) {
    if (ext_hex16_to_secret (hex + 2, out) < 0) {
      return -1;
    }
    if (((n - 34) & 1) != 0) {
      return -1;
    }
    for (i = 34; i < n; i++) {
      if (!isxdigit ((unsigned char) hex[i])) {
        return -1;
      }
    }
    return 0;
  }

  return -1;
}

static void ext_sanitize_label (const char *in, char out[EXT_SECRET_LABEL_MAX]) {
  int i = 0;
  if (!in) {
    out[0] = 0;
    return;
  }
  while (*in && i < EXT_SECRET_LABEL_MAX - 1) {
    unsigned char c = (unsigned char) *in++;
    if (isalnum (c) || c == '.' || c == '_' || c == '-' || c == ':') {
      out[i++] = (char) c;
    } else if (c == ' ' || c == '\t') {
      out[i++] = '_';
    }
  }
  out[i] = 0;
}

static int ext_is_active_locked (const struct ext_secret_entry *E, long long now) {
  if (!E->enabled) {
    return 0;
  }
  if (E->expires_at > 0 && now >= E->expires_at) {
    return 0;
  }
  return 1;
}

static int ext_find_idx_locked (const unsigned char secret[16]) {
  int i;
  for (i = 0; i < ext_secret_cnt; i++) {
    if (!memcmp (ext_secrets[i].secret, secret, 16)) {
      return i;
    }
  }
  return -1;
}

static int ext_reserve_locked (int want) {
  if (want <= ext_secret_cap) {
    return 0;
  }
  int new_cap = ext_secret_cap ? ext_secret_cap * 2 : 64;
  while (new_cap < want) {
    new_cap *= 2;
  }
  struct ext_secret_entry *p = realloc (ext_secrets, sizeof (*p) * new_cap);
  if (!p) {
    return -1;
  }
  ext_secrets = p;
  ext_secret_cap = new_cap;
  return 0;
}

static int ext_mkdirs_for_file (const char *path) {
  if (!path || !*path) {
    return 0;
  }
  char tmp[PATH_MAX];
  if (snprintf (tmp, sizeof (tmp), "%s", path) >= (int) sizeof (tmp)) {
    return -1;
  }
  char *p;
  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir (tmp, 0750) < 0 && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }
  return 0;
}

static int ext_persist_locked (void) {
  if (!ext_secret_state_file[0]) {
    return 0;
  }
  if (ext_mkdirs_for_file (ext_secret_state_file) < 0) {
    return -1;
  }

  char tmp_path[PATH_MAX];
  if (snprintf (tmp_path, sizeof (tmp_path), "%s.tmpXXXXXX", ext_secret_state_file) >= (int) sizeof (tmp_path)) {
    return -1;
  }
  int fd = mkstemp (tmp_path);
  if (fd < 0) {
    return -1;
  }

  FILE *f = fdopen (fd, "w");
  if (!f) {
    close (fd);
    unlink (tmp_path);
    return -1;
  }

  fprintf (f, "v1\n");
  int i;
  for (i = 0; i < ext_secret_cnt; i++) {
    struct ext_secret_entry *E = &ext_secrets[i];
    fprintf (f, "%s\t%d\t%lld\t%lld\t%lld\t%s\n",
             E->hex, E->enabled, E->expires_at, E->created_at, E->updated_at, E->label);
  }
  fflush (f);
  fsync (fileno (f));
  fclose (f);

  if (rename (tmp_path, ext_secret_state_file) < 0) {
    unlink (tmp_path);
    return -1;
  }
  return 0;
}

static int ext_add_locked (const unsigned char secret[16], const char *label, long long expires_at, int enabled, int fail_if_exists, int persist) {
  int idx = ext_find_idx_locked (secret);
  long long now = ext_now_sec();
  if (idx >= 0) {
    if (fail_if_exists) {
      return -1;
    }
    if (label && *label) {
      ext_sanitize_label (label, ext_secrets[idx].label);
    }
    if (expires_at >= 0) {
      ext_secrets[idx].expires_at = expires_at;
    }
    ext_secrets[idx].enabled = enabled ? 1 : 0;
    ext_secrets[idx].updated_at = now;
    return persist ? ext_persist_locked() : 0;
  }

  if (ext_reserve_locked (ext_secret_cnt + 1) < 0) {
    return -1;
  }
  struct ext_secret_entry *E = &ext_secrets[ext_secret_cnt++];
  memset (E, 0, sizeof (*E));
  memcpy (E->secret, secret, 16);
  ext_secret16_to_hex (secret, E->hex);
  ext_sanitize_label (label, E->label);
  E->enabled = enabled ? 1 : 0;
  E->created_at = now;
  E->updated_at = now;
  E->expires_at = expires_at > 0 ? expires_at : 0;
  return persist ? ext_persist_locked() : 0;
}

void tcp_rpcs_set_ext_secret (unsigned char secret[16]) {
  pthread_rwlock_wrlock (&ext_secret_lock);
  (void) ext_add_locked (secret, "bootstrap", 0, 1, 0, 0);
  pthread_rwlock_unlock (&ext_secret_lock);
}

void tcp_rpcs_set_secrets_state_file (const char *path) {
  pthread_rwlock_wrlock (&ext_secret_lock);
  if (path && *path) {
    snprintf (ext_secret_state_file, sizeof (ext_secret_state_file), "%s", path);
  } else {
    ext_secret_state_file[0] = 0;
  }
  pthread_rwlock_unlock (&ext_secret_lock);
}

int tcp_rpcs_load_secrets_state (void) {
  pthread_rwlock_wrlock (&ext_secret_lock);
  if (!ext_secret_state_file[0]) {
    pthread_rwlock_unlock (&ext_secret_lock);
    return 0;
  }
  FILE *f = fopen (ext_secret_state_file, "r");
  if (!f) {
    pthread_rwlock_unlock (&ext_secret_lock);
    return errno == ENOENT ? 0 : -1;
  }

  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  while ((n = getline (&line, &cap, f)) >= 0) {
    if (n <= 1 || line[0] == '#') {
      continue;
    }
    if (!strncmp (line, "v1", 2)) {
      continue;
    }
    char *saveptr = NULL;
    char *hex = strtok_r (line, "\t\r\n", &saveptr);
    char *enabled_s = strtok_r (NULL, "\t\r\n", &saveptr);
    char *expires_s = strtok_r (NULL, "\t\r\n", &saveptr);
    char *created_s = strtok_r (NULL, "\t\r\n", &saveptr);
    char *updated_s = strtok_r (NULL, "\t\r\n", &saveptr);
    char *label = strtok_r (NULL, "\t\r\n", &saveptr);
    if (!hex || !enabled_s || !expires_s || !created_s || !updated_s) {
      continue;
    }
    unsigned char secret[16];
    if (ext_secret16_from_hex (hex, secret) < 0) {
      continue;
    }
    int idx = ext_find_idx_locked (secret);
    if (idx >= 0) {
      struct ext_secret_entry *E = &ext_secrets[idx];
      E->enabled = atoi (enabled_s) ? 1 : 0;
      E->expires_at = atoll (expires_s);
      E->created_at = atoll (created_s);
      E->updated_at = atoll (updated_s);
      ext_sanitize_label (label ? label : "", E->label);
      continue;
    }
    if (ext_reserve_locked (ext_secret_cnt + 1) < 0) {
      break;
    }
    struct ext_secret_entry *E = &ext_secrets[ext_secret_cnt++];
    memset (E, 0, sizeof (*E));
    memcpy (E->secret, secret, 16);
    ext_secret16_to_hex (secret, E->hex);
    E->enabled = atoi (enabled_s) ? 1 : 0;
    E->expires_at = atoll (expires_s);
    E->created_at = atoll (created_s);
    E->updated_at = atoll (updated_s);
    ext_sanitize_label (label ? label : "", E->label);
  }
  free (line);
  fclose (f);
  pthread_rwlock_unlock (&ext_secret_lock);
  return 0;
}

int tcp_rpcs_total_secret_count (void) {
  int n;
  pthread_rwlock_rdlock (&ext_secret_lock);
  n = ext_secret_cnt;
  pthread_rwlock_unlock (&ext_secret_lock);
  return n;
}

int tcp_rpcs_active_secret_count (long long now) {
  int i, n = 0;
  pthread_rwlock_rdlock (&ext_secret_lock);
  for (i = 0; i < ext_secret_cnt; i++) {
    n += ext_is_active_locked (&ext_secrets[i], now);
  }
  pthread_rwlock_unlock (&ext_secret_lock);
  return n;
}

static int ext_snapshot_active_secrets (long long now, unsigned char **secrets, int *count) {
  *secrets = NULL;
  *count = 0;

  pthread_rwlock_rdlock (&ext_secret_lock);
  int i, n = 0;
  for (i = 0; i < ext_secret_cnt; i++) {
    n += ext_is_active_locked (&ext_secrets[i], now);
  }
  if (n <= 0) {
    pthread_rwlock_unlock (&ext_secret_lock);
    return 0;
  }

  unsigned char *tmp = malloc (16 * n);
  if (!tmp) {
    pthread_rwlock_unlock (&ext_secret_lock);
    return -1;
  }

  int j = 0;
  for (i = 0; i < ext_secret_cnt; i++) {
    if (!ext_is_active_locked (&ext_secrets[i], now)) {
      continue;
    }
    memcpy (tmp + 16 * j, ext_secrets[i].secret, 16);
    j++;
  }
  pthread_rwlock_unlock (&ext_secret_lock);

  *secrets = tmp;
  *count = n;
  return 0;
}

static int ext_parse_bool (const char *s, int def) {
  if (!s || !*s) {
    return def;
  }
  if (!strcmp (s, "1") || !strcasecmp (s, "true") || !strcasecmp (s, "yes")) {
    return 1;
  }
  if (!strcmp (s, "0") || !strcasecmp (s, "false") || !strcasecmp (s, "no")) {
    return 0;
  }
  return def;
}

static int ext_write_all (int fd, const char *buf, int len) {
  while (len > 0) {
    int r = send (fd, buf, len, 0);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    buf += r;
    len -= r;
  }
  return 0;
}

static int ext_cmd_add (const char *hex, const char *label, long long expires_at, int enabled) {
  unsigned char secret[16];
  if (ext_secret16_from_hex (hex, secret) < 0) {
    return -2;
  }
  pthread_rwlock_wrlock (&ext_secret_lock);
  int r = ext_add_locked (secret, label, expires_at, enabled, 1, 1);
  pthread_rwlock_unlock (&ext_secret_lock);
  return r;
}

static int ext_cmd_remove (const char *hex) {
  unsigned char secret[16];
  if (ext_secret16_from_hex (hex, secret) < 0) {
    return -2;
  }
  pthread_rwlock_wrlock (&ext_secret_lock);
  int idx = ext_find_idx_locked (secret);
  if (idx < 0) {
    pthread_rwlock_unlock (&ext_secret_lock);
    return -3;
  }
  ext_secrets[idx] = ext_secrets[ext_secret_cnt - 1];
  ext_secret_cnt--;
  int r = ext_persist_locked();
  pthread_rwlock_unlock (&ext_secret_lock);
  return r;
}

static int ext_cmd_set_enabled (const char *hex, int enabled) {
  unsigned char secret[16];
  if (ext_secret16_from_hex (hex, secret) < 0) {
    return -2;
  }
  pthread_rwlock_wrlock (&ext_secret_lock);
  int idx = ext_find_idx_locked (secret);
  if (idx < 0) {
    pthread_rwlock_unlock (&ext_secret_lock);
    return -3;
  }
  ext_secrets[idx].enabled = enabled ? 1 : 0;
  ext_secrets[idx].updated_at = ext_now_sec();
  int r = ext_persist_locked();
  pthread_rwlock_unlock (&ext_secret_lock);
  return r;
}

static int ext_cmd_disable_expired (long long now, int *checked_out, int *disabled_out, long long *now_out) {
  if (now <= 0) {
    now = ext_now_sec();
  }
  int checked = 0;
  int disabled = 0;
  int changed = 0;

  pthread_rwlock_wrlock (&ext_secret_lock);
  int i;
  for (i = 0; i < ext_secret_cnt; i++) {
    struct ext_secret_entry *E = &ext_secrets[i];
    checked++;
    if (E->enabled && E->expires_at > 0 && now >= E->expires_at) {
      E->enabled = 0;
      E->updated_at = now;
      disabled++;
      changed = 1;
    }
  }
  int r = 0;
  if (changed) {
    r = ext_persist_locked();
  }
  pthread_rwlock_unlock (&ext_secret_lock);

  if (checked_out) {
    *checked_out = checked;
  }
  if (disabled_out) {
    *disabled_out = disabled;
  }
  if (now_out) {
    *now_out = now;
  }
  return r;
}

static void ext_admin_send_err (int fd, const char *msg) {
  char buf[256];
  int n = snprintf (buf, sizeof (buf), "ERR %s\n", msg ? msg : "error");
  ext_write_all (fd, buf, n);
}

static const char *ext_find_kv (int n, char **keys, char **vals, const char *key) {
  int i;
  for (i = 0; i < n; i++) {
    if (!strcmp (keys[i], key)) {
      return vals[i];
    }
  }
  return NULL;
}

struct ext_strbuf {
  char *s;
  size_t len;
  size_t cap;
};

static int ext_sb_grow (struct ext_strbuf *B, size_t need_extra) {
  if (B->len + need_extra + 1 <= B->cap) {
    return 0;
  }
  size_t new_cap = B->cap ? B->cap : 512;
  while (new_cap < B->len + need_extra + 1) {
    new_cap *= 2;
  }
  char *p = realloc (B->s, new_cap);
  if (!p) {
    return -1;
  }
  B->s = p;
  B->cap = new_cap;
  return 0;
}

static int ext_sb_appendf (struct ext_strbuf *B, const char *fmt, ...) {
  va_list ap;
  va_start (ap, fmt);
  va_list aq;
  va_copy (aq, ap);
  int n = vsnprintf (NULL, 0, fmt, aq);
  va_end (aq);
  if (n < 0) {
    va_end (ap);
    return -1;
  }
  if (ext_sb_grow (B, (size_t) n) < 0) {
    va_end (ap);
    return -1;
  }
  vsnprintf (B->s + B->len, B->cap - B->len, fmt, ap);
  va_end (ap);
  B->len += (size_t) n;
  return 0;
}

static void ext_sb_free (struct ext_strbuf *B) {
  free (B->s);
  B->s = NULL;
  B->len = B->cap = 0;
}

static const char *ext_http_status_text (int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

static int ext_http_send_json (int fd, int code, const char *body) {
  if (!body) {
    body = "{}";
  }
  char header[512];
  int h = snprintf (header, sizeof (header),
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    code, ext_http_status_text (code), strlen (body));
  if (h <= 0 || h >= (int) sizeof (header)) {
    return -1;
  }
  if (ext_write_all (fd, header, h) < 0) {
    return -1;
  }
  return ext_write_all (fd, body, (int) strlen (body));
}

static int ext_http_send_error (int fd, int code, const char *msg) {
  struct ext_strbuf B = {};
  if (ext_sb_appendf (&B, "{\"error\":\"%s\"}", msg ? msg : "error") < 0) {
    ext_sb_free (&B);
    return -1;
  }
  int r = ext_http_send_json (fd, code, B.s);
  ext_sb_free (&B);
  return r;
}

static int ext_http_get_header_value (const char *headers, const char *name, char *out, int out_size) {
  if (!headers || !name || !out || out_size <= 0) {
    return 0;
  }
  char pattern[128];
  if (snprintf (pattern, sizeof (pattern), "\r\n%s:", name) >= (int) sizeof (pattern)) {
    return 0;
  }
  const char *p = strcasestr (headers, pattern);
  if (p) {
    p += 2;
  } else {
    if (!strncasecmp (headers, name, strlen (name)) && headers[strlen (name)] == ':') {
      p = headers;
    } else {
      return 0;
    }
  }
  p += strlen (name) + 1;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  int i = 0;
  while (*p && *p != '\r' && *p != '\n' && i < out_size - 1) {
    out[i++] = *p++;
  }
  out[i] = 0;
  return i > 0;
}

static int ext_http_json_find_key (const char *body, const char *key, const char **value_start) {
  char pattern[128];
  if (snprintf (pattern, sizeof (pattern), "\"%s\"", key) >= (int) sizeof (pattern)) {
    return 0;
  }
  const char *p = strstr (body, pattern);
  if (!p) {
    return 0;
  }
  p += strlen (pattern);
  while (*p && isspace ((unsigned char) *p)) {
    p++;
  }
  if (*p != ':') {
    return 0;
  }
  p++;
  while (*p && isspace ((unsigned char) *p)) {
    p++;
  }
  *value_start = p;
  return 1;
}

static int ext_http_json_get_string (const char *body, const char *key, char *out, int out_size) {
  const char *p;
  if (!ext_http_json_find_key (body, key, &p)) {
    return 0;
  }
  if (*p != '"' || out_size <= 0) {
    return 0;
  }
  p++;
  int i = 0;
  while (*p && *p != '"' && i < out_size - 1) {
    if (*p == '\\' && p[1]) {
      p++;
    }
    out[i++] = *p++;
  }
  if (*p != '"') {
    return 0;
  }
  out[i] = 0;
  return 1;
}

static int ext_http_json_get_int64 (const char *body, const char *key, long long *out, int *found) {
  *found = 0;
  const char *p;
  if (!ext_http_json_find_key (body, key, &p)) {
    return 1;
  }
  char *end = NULL;
  errno = 0;
  long long v = strtoll (p, &end, 10);
  if (errno || end == p) {
    return 0;
  }
  *out = v;
  *found = 1;
  return 1;
}

static int ext_http_json_get_bool (const char *body, const char *key, int *out, int *found) {
  *found = 0;
  const char *p;
  if (!ext_http_json_find_key (body, key, &p)) {
    return 1;
  }
  if (!strncasecmp (p, "true", 4)) {
    *out = 1;
    *found = 1;
    return 1;
  }
  if (!strncasecmp (p, "false", 5)) {
    *out = 0;
    *found = 1;
    return 1;
  }
  if (*p == '1') {
    *out = 1;
    *found = 1;
    return 1;
  }
  if (*p == '0') {
    *out = 0;
    *found = 1;
    return 1;
  }
  return 0;
}

static int ext_http_append_status_json (struct ext_strbuf *B) {
  long long now = ext_now_sec();
  int total = tcp_rpcs_total_secret_count();
  int active = tcp_rpcs_active_secret_count (now);
  return ext_sb_appendf (B, "{\"ok\":true,\"total\":%d,\"active\":%d,\"now\":%lld}", total, active, now);
}

static int ext_http_append_list_json (struct ext_strbuf *B) {
  long long now = ext_now_sec();
  if (ext_sb_appendf (B, "{\"ok\":true,\"secrets\":[") < 0) {
    return -1;
  }
  pthread_rwlock_rdlock (&ext_secret_lock);
  int i;
  for (i = 0; i < ext_secret_cnt; i++) {
    struct ext_secret_entry *E = &ext_secrets[i];
    int active = ext_is_active_locked (E, now);
    if (i && ext_sb_appendf (B, ",") < 0) {
      pthread_rwlock_unlock (&ext_secret_lock);
      return -1;
    }
    if (ext_sb_appendf (B,
                        "{\"secret\":\"%s\",\"enabled\":%s,\"active\":%s,"
                        "\"expires\":%lld,\"created\":%lld,\"updated\":%lld,\"label\":\"%s\"}",
                        E->hex, E->enabled ? "true" : "false", active ? "true" : "false",
                        E->expires_at, E->created_at, E->updated_at, E->label) < 0) {
      pthread_rwlock_unlock (&ext_secret_lock);
      return -1;
    }
  }
  pthread_rwlock_unlock (&ext_secret_lock);
  return ext_sb_appendf (B, "]}");
}

static void ext_admin_handle_line (int fd, char *line) {
  char *newline = strchr (line, '\n');
  if (newline) {
    *newline = 0;
  }
  char *saveptr = NULL;
  char *cmd = strtok_r (line, " \t\r", &saveptr);
  if (!cmd) {
    ext_admin_send_err (fd, "empty command");
    return;
  }

  char *keys[64], *vals[64];
  int kvn = 0;
  char *tok;
  while ((tok = strtok_r (NULL, " \t\r", &saveptr)) != NULL && kvn < 64) {
    char *eq = strchr (tok, '=');
    if (!eq || eq == tok || !eq[1]) {
      continue;
    }
    *eq = 0;
    keys[kvn] = tok;
    vals[kvn] = eq + 1;
    kvn++;
  }

  if (ext_secret_admin_token[0]) {
    const char *token = ext_find_kv (kvn, keys, vals, "token");
    if (!token || strcmp (token, ext_secret_admin_token)) {
      ext_admin_send_err (fd, "unauthorized");
      return;
    }
  }

  if (!strcasecmp (cmd, "PING")) {
    ext_write_all (fd, "OK PONG\n", 8);
    return;
  }
  if (!strcasecmp (cmd, "STATUS")) {
    long long now = ext_now_sec();
    char out[256];
    int n = snprintf (out, sizeof (out), "OK total=%d active=%d now=%lld\n",
                      tcp_rpcs_total_secret_count(), tcp_rpcs_active_secret_count (now), now);
    ext_write_all (fd, out, n);
    return;
  }
  if (!strcasecmp (cmd, "LIST")) {
    pthread_rwlock_rdlock (&ext_secret_lock);
    long long now = ext_now_sec();
    char out[512];
    int i, active = 0;
    for (i = 0; i < ext_secret_cnt; i++) {
      active += ext_is_active_locked (&ext_secrets[i], now);
    }
    int n = snprintf (out, sizeof (out), "OK total=%d active=%d\n", ext_secret_cnt, active);
    ext_write_all (fd, out, n);
    for (i = 0; i < ext_secret_cnt; i++) {
      struct ext_secret_entry *E = &ext_secrets[i];
      n = snprintf (out, sizeof (out),
                    "secret=%s enabled=%d expires=%lld created=%lld updated=%lld label=%s\n",
                    E->hex, E->enabled, E->expires_at, E->created_at, E->updated_at, E->label);
      ext_write_all (fd, out, n);
    }
    pthread_rwlock_unlock (&ext_secret_lock);
    ext_write_all (fd, ".\n", 2);
    return;
  }
  if (!strcasecmp (cmd, "EXPIRE_DISABLE") || !strcasecmp (cmd, "SYNC_EXPIRED")) {
    const char *now_s = ext_find_kv (kvn, keys, vals, "now");
    long long now = now_s ? atoll (now_s) : 0;
    int checked = 0, disabled = 0;
    long long used_now = 0;
    int rc = ext_cmd_disable_expired (now, &checked, &disabled, &used_now);
    if (rc < 0) {
      ext_admin_send_err (fd, "update failed");
    } else {
      char out[256];
      int n = snprintf (out, sizeof (out), "OK checked=%d disabled=%d now=%lld\n", checked, disabled, used_now);
      ext_write_all (fd, out, n);
    }
    return;
  }
  if (!strcasecmp (cmd, "ADD")) {
    const char *secret = ext_find_kv (kvn, keys, vals, "secret");
    if (!secret) {
      ext_admin_send_err (fd, "secret is required");
      return;
    }
    const char *label = ext_find_kv (kvn, keys, vals, "label");
    const char *expires_s = ext_find_kv (kvn, keys, vals, "expires");
    const char *enabled_s = ext_find_kv (kvn, keys, vals, "enabled");
    long long expires = expires_s ? atoll (expires_s) : 0;
    int enabled = ext_parse_bool (enabled_s, 1);
    int rc = ext_cmd_add (secret, label ? label : "", expires, enabled);
    if (rc == -2) {
      ext_admin_send_err (fd, "invalid secret format");
    } else if (rc < 0) {
      ext_admin_send_err (fd, "add failed");
    } else {
      ext_write_all (fd, "OK\n", 3);
    }
    return;
  }
  if (!strcasecmp (cmd, "REMOVE")) {
    const char *secret = ext_find_kv (kvn, keys, vals, "secret");
    if (!secret) {
      ext_admin_send_err (fd, "secret is required");
      return;
    }
    int rc = ext_cmd_remove (secret);
    if (rc == -2) {
      ext_admin_send_err (fd, "invalid secret format");
    } else if (rc == -3) {
      ext_admin_send_err (fd, "secret not found");
    } else if (rc < 0) {
      ext_admin_send_err (fd, "remove failed");
    } else {
      ext_write_all (fd, "OK\n", 3);
    }
    return;
  }
  if (!strcasecmp (cmd, "ENABLE") || !strcasecmp (cmd, "DISABLE")) {
    const char *secret = ext_find_kv (kvn, keys, vals, "secret");
    if (!secret) {
      ext_admin_send_err (fd, "secret is required");
      return;
    }
    int enable = !strcasecmp (cmd, "ENABLE");
    int rc = ext_cmd_set_enabled (secret, enable);
    if (rc == -2) {
      ext_admin_send_err (fd, "invalid secret format");
    } else if (rc == -3) {
      ext_admin_send_err (fd, "secret not found");
    } else if (rc < 0) {
      ext_admin_send_err (fd, "update failed");
    } else {
      ext_write_all (fd, "OK\n", 3);
    }
    return;
  }

  ext_admin_send_err (fd, "unknown command");
}

static int ext_http_parse_content_length (const char *headers) {
  char val[64];
  if (!ext_http_get_header_value (headers, "Content-Length", val, sizeof (val))) {
    return 0;
  }
  int n = atoi (val);
  if (n < 0) {
    n = 0;
  }
  return n;
}

static int ext_http_extract_token_from_uri (const char *uri, char *out, int out_size) {
  const char *q = strchr (uri, '?');
  if (!q) {
    return 0;
  }
  q++;
  while (*q) {
    const char *amp = strchr (q, '&');
    size_t len = amp ? (size_t) (amp - q) : strlen (q);
    if (len > 6 && !strncmp (q, "token=", 6)) {
      size_t i, m = len - 6;
      if ((int) m >= out_size) {
        m = out_size - 1;
      }
      for (i = 0; i < m; i++) {
        out[i] = q[6 + i];
      }
      out[m] = 0;
      return 1;
    }
    if (!amp) {
      break;
    }
    q = amp + 1;
  }
  return 0;
}

static void ext_admin_handle_http (int fd, char *req) {
  char *headers_end = strstr (req, "\r\n\r\n");
  if (!headers_end) {
    ext_http_send_error (fd, 400, "invalid http request");
    return;
  }
  *headers_end = 0;
  char *body = headers_end + 4;

  char method[16], uri[1024], version[16];
  if (sscanf (req, "%15s %1023s %15s", method, uri, version) != 3) {
    ext_http_send_error (fd, 400, "invalid request line");
    return;
  }

  if (ext_secret_admin_token[0]) {
    char header_token[EXT_SECRET_ADMIN_TOKEN_MAX];
    int ok = ext_http_get_header_value (req, "X-Admin-Token", header_token, sizeof (header_token));
    if (!ok) {
      ok = ext_http_extract_token_from_uri (uri, header_token, sizeof (header_token));
    }
    if (!ok || strcmp (header_token, ext_secret_admin_token)) {
      ext_http_send_error (fd, 401, "unauthorized");
      return;
    }
  }

  char path[1024];
  snprintf (path, sizeof (path), "%s", uri);
  char *q = strchr (path, '?');
  if (q) {
    *q = 0;
  }

  if (!strcmp (method, "GET") && !strcmp (path, "/v1/status")) {
    struct ext_strbuf B = {};
    if (ext_http_append_status_json (&B) < 0) {
      ext_sb_free (&B);
      ext_http_send_error (fd, 500, "oom");
      return;
    }
    ext_http_send_json (fd, 200, B.s);
    ext_sb_free (&B);
    return;
  }

  if (!strcmp (method, "GET") && !strcmp (path, "/v1/secrets")) {
    struct ext_strbuf B = {};
    if (ext_http_append_list_json (&B) < 0) {
      ext_sb_free (&B);
      ext_http_send_error (fd, 500, "oom");
      return;
    }
    ext_http_send_json (fd, 200, B.s);
    ext_sb_free (&B);
    return;
  }

  if (!strcmp (method, "POST") && !strcmp (path, "/v1/secrets")) {
    char secret[64], label[EXT_SECRET_LABEL_MAX];
    secret[0] = label[0] = 0;
    if (!ext_http_json_get_string (body, "secret", secret, sizeof (secret))) {
      ext_http_send_error (fd, 400, "secret is required");
      return;
    }
    ext_http_json_get_string (body, "label", label, sizeof (label));
    long long expires = 0;
    int found_expires = 0;
    if (!ext_http_json_get_int64 (body, "expires", &expires, &found_expires)) {
      ext_http_send_error (fd, 400, "invalid expires");
      return;
    }
    if (!found_expires && !ext_http_json_get_int64 (body, "expires_at", &expires, &found_expires)) {
      ext_http_send_error (fd, 400, "invalid expires_at");
      return;
    }
    int enabled = 1;
    int found_enabled = 0;
    if (!ext_http_json_get_bool (body, "enabled", &enabled, &found_enabled)) {
      ext_http_send_error (fd, 400, "invalid enabled");
      return;
    }
    int rc = ext_cmd_add (secret, label, found_expires ? expires : 0, enabled);
    if (rc == -2) {
      ext_http_send_error (fd, 400, "invalid secret format");
    } else if (rc < 0) {
      ext_http_send_error (fd, 409, "add failed");
    } else {
      ext_http_send_json (fd, 201, "{\"ok\":true}");
    }
    return;
  }

  if (!strcmp (method, "POST") && !strcmp (path, "/v1/secrets/expire_disable")) {
    long long now = 0;
    int found_now = 0;
    if (!ext_http_json_get_int64 (body, "now", &now, &found_now)) {
      ext_http_send_error (fd, 400, "invalid now");
      return;
    }
    int checked = 0, disabled = 0;
    long long used_now = 0;
    int rc = ext_cmd_disable_expired (found_now ? now : 0, &checked, &disabled, &used_now);
    if (rc < 0) {
      ext_http_send_error (fd, 500, "update failed");
      return;
    }
    struct ext_strbuf B = {};
    if (ext_sb_appendf (&B, "{\"ok\":true,\"checked\":%d,\"disabled\":%d,\"now\":%lld}", checked, disabled, used_now) < 0) {
      ext_sb_free (&B);
      ext_http_send_error (fd, 500, "oom");
      return;
    }
    ext_http_send_json (fd, 200, B.s);
    ext_sb_free (&B);
    return;
  }

  if (!strcmp (method, "DELETE") && !strncmp (path, "/v1/secrets/", 12)) {
    const char *secret = path + 12;
    if (!*secret) {
      ext_http_send_error (fd, 400, "secret is required");
      return;
    }
    int rc = ext_cmd_remove (secret);
    if (rc == -2) {
      ext_http_send_error (fd, 400, "invalid secret format");
    } else if (rc == -3) {
      ext_http_send_error (fd, 404, "secret not found");
    } else if (rc < 0) {
      ext_http_send_error (fd, 500, "remove failed");
    } else {
      ext_http_send_json (fd, 200, "{\"ok\":true}");
    }
    return;
  }

  if (!strcmp (method, "PATCH") && !strncmp (path, "/v1/secrets/", 12)) {
    char tmp[1024];
    snprintf (tmp, sizeof (tmp), "%s", path + 12);
    char *slash = strchr (tmp, '/');
    if (!slash) {
      ext_http_send_error (fd, 404, "not found");
      return;
    }
    *slash = 0;
    const char *secret = tmp;
    const char *action = slash + 1;
    int enable;
    if (!strcmp (action, "enable")) {
      enable = 1;
    } else if (!strcmp (action, "disable")) {
      enable = 0;
    } else {
      ext_http_send_error (fd, 404, "not found");
      return;
    }
    int rc = ext_cmd_set_enabled (secret, enable);
    if (rc == -2) {
      ext_http_send_error (fd, 400, "invalid secret format");
    } else if (rc == -3) {
      ext_http_send_error (fd, 404, "secret not found");
    } else if (rc < 0) {
      ext_http_send_error (fd, 500, "update failed");
    } else {
      ext_http_send_json (fd, 200, "{\"ok\":true}");
    }
    return;
  }

  ext_http_send_error (fd, 404, "not found");
}

static int ext_is_http_method_prefix (const char *buf) {
  return !strncmp (buf, "GET ", 4) ||
         !strncmp (buf, "POST ", 5) ||
         !strncmp (buf, "PATCH ", 6) ||
         !strncmp (buf, "DELETE ", 7);
}

static void ext_admin_handle_client (int fd) {
  char buf[16384];
  int len = 0;
  int r;
  while (len < (int) sizeof (buf) - 1) {
    r = recv (fd, buf + len, sizeof (buf) - 1 - len, 0);
    if (r <= 0) {
      break;
    }
    len += r;
    buf[len] = 0;
    if (!ext_is_http_method_prefix (buf)) {
      if (strchr (buf, '\n')) {
        break;
      }
      continue;
    }
    char *headers_end = strstr (buf, "\r\n\r\n");
    if (!headers_end) {
      continue;
    }
    int content_length = ext_http_parse_content_length (buf);
    int need = (int) ((headers_end + 4) - buf) + content_length;
    if (len >= need) {
      break;
    }
  }
  if (len <= 0) {
    return;
  }
  buf[len] = 0;

  if (ext_is_http_method_prefix (buf)) {
    ext_admin_handle_http (fd, buf);
    return;
  }
  ext_admin_handle_line (fd, buf);
}

static void *ext_admin_thread_main (void *arg) {
  (void) arg;
  int fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return NULL;
  }

  if (ext_mkdirs_for_file (ext_secret_admin_socket) < 0) {
    close (fd);
    return NULL;
  }

  unlink (ext_secret_admin_socket);
  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  size_t sun_path_len = strlen (ext_secret_admin_socket);
  if (sun_path_len >= sizeof (addr.sun_path)) {
    close (fd);
    return NULL;
  }
  memcpy (addr.sun_path, ext_secret_admin_socket, sun_path_len);
  addr.sun_path[sun_path_len] = 0;

  socklen_t bind_len = (socklen_t) (offsetof (struct sockaddr_un, sun_path) + sun_path_len + 1);
  if (bind (fd, (struct sockaddr *) &addr, bind_len) < 0) {
    close (fd);
    return NULL;
  }
  chmod (ext_secret_admin_socket, 0660);
  if (listen (fd, 128) < 0) {
    close (fd);
    return NULL;
  }

  while (1) {
    int cfd = accept (fd, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR) {
        continue;
      }
      sleep (1);
      continue;
    }
    ext_admin_handle_client (cfd);
    close (cfd);
  }
  return NULL;
}

int tcp_rpcs_start_admin_server (const char *socket_path, const char *token) {
  if (!socket_path || !*socket_path) {
    return 0;
  }
  if (strlen (socket_path) >= sizeof (((struct sockaddr_un *)0)->sun_path)) {
    return -1;
  }
  pthread_rwlock_wrlock (&ext_secret_lock);
  if (ext_secret_admin_started) {
    pthread_rwlock_unlock (&ext_secret_lock);
    return 0;
  }
  snprintf (ext_secret_admin_socket, sizeof (ext_secret_admin_socket), "%s", socket_path);
  if (token && *token) {
    snprintf (ext_secret_admin_token, sizeof (ext_secret_admin_token), "%s", token);
  } else {
    ext_secret_admin_token[0] = 0;
  }
  ext_secret_admin_started = 1;
  pthread_rwlock_unlock (&ext_secret_lock);

  if (pthread_create (&ext_secret_admin_thread, NULL, ext_admin_thread_main, NULL) != 0) {
    pthread_rwlock_wrlock (&ext_secret_lock);
    ext_secret_admin_started = 0;
    pthread_rwlock_unlock (&ext_secret_lock);
    return -1;
  }
  pthread_detach (ext_secret_admin_thread);
  return 0;
}

static int allow_only_tls;

struct domain_info {
  const char *domain;
  struct in_addr target;
  unsigned char target_ipv6[16];
  short server_hello_encrypted_size;
  char use_random_encrypted_size;
  char is_reversed_extension_order;
  struct domain_info *next;
};

static struct domain_info *default_domain_info;

#define DOMAIN_HASH_MOD 257
static struct domain_info *domains[DOMAIN_HASH_MOD];

static struct domain_info **get_domain_info_bucket (const char *domain, size_t len) {
  size_t i;
  unsigned hash = 0;
  for (i = 0; i < len; i++) {
    hash = hash * 239017 + (unsigned char)domain[i];
  }
  return domains + hash % DOMAIN_HASH_MOD;
}

static const struct domain_info *get_domain_info (const char *domain, size_t len) {
  struct domain_info *info = *get_domain_info_bucket (domain, len);
  while (info != NULL) {
    if (strlen (info->domain) == len && memcmp (domain, info->domain, len) == 0) {
      return info;
    }
    info = info->next;
  }
  return NULL;
}

static int get_domain_server_hello_encrypted_size (const struct domain_info *info) {
  if (info->use_random_encrypted_size) {
    int r = rand();
    return info->server_hello_encrypted_size + ((r >> 1) & 1) - (r & 1);
  } else {
    return info->server_hello_encrypted_size;
  }
}

#define TLS_REQUEST_LENGTH 517

static BIGNUM *get_y2 (BIGNUM *x, const BIGNUM *mod, BN_CTX *big_num_context) {
  // returns y^2 = x^3 + 486662 * x^2 + x
  BIGNUM *y = BN_dup (x);
  assert (y != NULL);
  BIGNUM *coef = BN_new();
  assert (BN_set_word (coef, 486662) == 1);
  assert (BN_mod_add (y, y, coef, mod, big_num_context) == 1);
  assert (BN_mod_mul (y, y, x, mod, big_num_context) == 1);
  assert (BN_one (coef) == 1);
  assert (BN_mod_add (y, y, coef, mod, big_num_context) == 1);
  assert (BN_mod_mul (y, y, x, mod, big_num_context) == 1);
  BN_clear_free (coef);
  return y;
}

static BIGNUM *get_double_x (BIGNUM *x, const BIGNUM *mod, BN_CTX *big_num_context) {
  // returns x_2 = (x^2 - 1)^2/(4*y^2)
  BIGNUM *denominator = get_y2 (x, mod, big_num_context);
  assert (denominator != NULL);
  BIGNUM *coef = BN_new();
  assert (BN_set_word (coef, 4) == 1);
  assert (BN_mod_mul (denominator, denominator, coef, mod, big_num_context) == 1);

  BIGNUM *numerator = BN_new();
  assert (numerator != NULL);
  assert (BN_mod_mul (numerator, x, x, mod, big_num_context) == 1);
  assert (BN_one (coef) == 1);
  assert (BN_mod_sub (numerator, numerator, coef, mod, big_num_context) == 1);
  assert (BN_mod_mul (numerator, numerator, numerator, mod, big_num_context) == 1);

  assert (BN_mod_inverse (denominator, denominator, mod, big_num_context) == denominator);
  assert (BN_mod_mul (numerator, numerator, denominator, mod, big_num_context) == 1);

  BN_clear_free (coef);
  BN_clear_free (denominator);
  return numerator;
}

static void generate_public_key (unsigned char key[32]) {
  BIGNUM *mod = NULL;
  assert (BN_hex2bn (&mod, "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed") == 64);
  BIGNUM *pow = NULL;
  assert (BN_hex2bn (&pow, "3ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6") == 64);
  BN_CTX *big_num_context = BN_CTX_new();
  assert (big_num_context != NULL);

  BIGNUM *x = BN_new();
  while (1) {
    assert (RAND_bytes (key, 32) == 1);
    key[31] &= 127;
    BN_bin2bn (key, 32, x);
    assert (x != NULL);
    assert (BN_mod_mul (x, x, x, mod, big_num_context) == 1);

    BIGNUM *y = get_y2 (x, mod, big_num_context);

    BIGNUM *r = BN_new();
    assert (BN_mod_exp (r, y, pow, mod, big_num_context) == 1);
    BN_clear_free (y);
    if (BN_is_one (r)) {
      BN_clear_free (r);
      break;
    }
    BN_clear_free (r);
  }

  int i;
  for (i = 0; i < 3; i++) {
    BIGNUM *x2 = get_double_x (x, mod, big_num_context);
    BN_clear_free (x);
    x = x2;
  }

  int num_size = BN_num_bytes (x);
  assert (num_size <= 32);
  memset (key, '\0', 32 - num_size);
  assert (BN_bn2bin (x, key + (32 - num_size)) == num_size);
  for (i = 0; i < 16; i++) {
    unsigned char t = key[i];
    key[i] = key[31 - i];
    key[31 - i] = t;
  }

  BN_clear_free (x);
  BN_CTX_free (big_num_context);
  BN_clear_free (pow);
  BN_clear_free (mod);
}

static void add_string (unsigned char *str, int *pos, const char *data, int data_len) {
  assert (*pos + data_len <= TLS_REQUEST_LENGTH);
  memcpy (str + (*pos), data, data_len);
  (*pos) += data_len;
}

static void add_random (unsigned char *str, int *pos, int random_len) {
  assert (*pos + random_len <= TLS_REQUEST_LENGTH);
  assert (RAND_bytes (str + (*pos), random_len) == 1);
  (*pos) += random_len;
}

static void add_length (unsigned char *str, int *pos, int length) {
  assert (*pos + 2 <= TLS_REQUEST_LENGTH);
  str[*pos + 0] = (unsigned char)(length / 256);
  str[*pos + 1] = (unsigned char)(length % 256);
  (*pos) += 2;
}

static void add_grease (unsigned char *str, int *pos, const unsigned char *greases, int num) {
  assert (*pos + 2 <= TLS_REQUEST_LENGTH);
  str[*pos + 0] = greases[num];
  str[*pos + 1] = greases[num];
  (*pos) += 2;
}

static void add_public_key (unsigned char *str, int *pos) {
  assert (*pos + 32 <= TLS_REQUEST_LENGTH);
  generate_public_key (str + (*pos));
  (*pos) += 32;
}

static unsigned char *create_request (const char *domain) {
  unsigned char *result = malloc (TLS_REQUEST_LENGTH);
  int pos = 0;

#define MAX_GREASE 7
  unsigned char greases[MAX_GREASE];
  assert (RAND_bytes (greases, MAX_GREASE) == 1);
  int i;
  for (i = 0; i < MAX_GREASE; i++) {
    greases[i] = (unsigned char)((greases[i] & 0xF0) + 0x0A);
  }
  for (i = 1; i < MAX_GREASE; i += 2) {
    if (greases[i] == greases[i - 1]) {
      greases[i] = (unsigned char)(0x10 ^ greases[i]);
    }
  }
#undef MAX_GREASE

  int domain_length = (int)strlen (domain);

  add_string (result, &pos, "\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11);
  add_random (result, &pos, 32);
  add_string (result, &pos, "\x20", 1);
  add_random (result, &pos, 32);
  add_string (result, &pos, "\x00\x22", 2);
  add_grease (result, &pos, greases, 0);
  add_string (result, &pos, "\x13\x01\x13\x02\x13\x03\xc0\x2b\xc0\x2f\xc0\x2c\xc0\x30\xcc\xa9\xcc\xa8"
                            "\xc0\x13\xc0\x14\x00\x9c\x00\x9d\x00\x2f\x00\x35\x00\x0a\x01\x00\x01\x91", 36);
  add_grease (result, &pos, greases, 2);
  add_string (result, &pos, "\x00\x00\x00\x00", 4);
  add_length (result, &pos, domain_length + 5);
  add_length (result, &pos, domain_length + 3);
  add_string (result, &pos, "\x00", 1);
  add_length (result, &pos, domain_length);
  add_string (result, &pos, domain, domain_length);
  add_string (result, &pos, "\x00\x17\x00\x00\xff\x01\x00\x01\x00\x00\x0a\x00\x0a\x00\x08", 15);
  add_grease (result, &pos, greases, 4);
  add_string (result, &pos, "\x00\x1d\x00\x17\x00\x18\x00\x0b\x00\x02\x01\x00\x00\x23\x00\x00\x00\x10"
                            "\x00\x0e\x00\x0c\x02\x68\x32\x08\x68\x74\x74\x70\x2f\x31\x2e\x31\x00\x05"
                            "\x00\x05\x01\x00\x00\x00\x00\x00\x0d\x00\x14\x00\x12\x04\x03\x08\x04\x04"
                            "\x01\x05\x03\x08\x05\x05\x01\x08\x06\x06\x01\x02\x01\x00\x12\x00\x00\x00"
                            "\x33\x00\x2b\x00\x29", 77);
  add_grease (result, &pos, greases, 4);
  add_string (result, &pos, "\x00\x01\x00\x00\x1d\x00\x20", 7);
  add_public_key (result, &pos);
  add_string (result, &pos, "\x00\x2d\x00\x02\x01\x01\x00\x2b\x00\x0b\x0a", 11);
  add_grease (result, &pos, greases, 6);
  add_string (result, &pos, "\x03\x04\x03\x03\x03\x02\x03\x01\x00\x1b\x00\x03\x02\x00\x02", 15);
  add_grease (result, &pos, greases, 3);
  add_string (result, &pos, "\x00\x01\x00\x00\x15", 5);

  int padding_length = TLS_REQUEST_LENGTH - 2 - pos;
  assert (padding_length >= 0);
  add_length (result, &pos, padding_length);
  memset (result + pos, 0, TLS_REQUEST_LENGTH - pos);
  return result;
}

static int read_length (const unsigned char *response, int *pos) {
  *pos += 2;
  return response[*pos - 2] * 256 + response[*pos - 1];
}

static int check_response (const unsigned char *response, int len, const unsigned char *request_session_id, int *is_reversed_extension_order, int *encrypted_application_data_length) {
#define FAIL(error) {                                               \
    kprintf ("Failed to parse upstream TLS response: " error "\n"); \
    return 0;                                                       \
  }
#define CHECK_LENGTH(length)  \
  if (pos + (length) > len) { \
    FAIL("Too short");        \
  }
#define EXPECT_STR(pos, str, error)                          \
  if (memcmp (response + pos, str, sizeof (str) - 1) != 0) { \
    FAIL(error);                                             \
  }

  int pos = 0;
  CHECK_LENGTH(3);
  EXPECT_STR(0, "\x16\x03\x03", "Non-TLS response or TLS <= 1.1");
  pos += 3;
  CHECK_LENGTH(2);
  int server_hello_length = read_length (response, &pos);
  if (server_hello_length <= 39) {
    FAIL("Receive too short ServerHello");
  }
  CHECK_LENGTH(server_hello_length);

  EXPECT_STR(5, "\x02\x00", "Non-TLS response 2");
  EXPECT_STR(9, "\x03\x03", "Non-TLS response 3");

  if (memcmp (response + 11, "\xcf\x21\xad\x74\xe5\x9a\x61\x11\xbe\x1d\x8c\x02\x1e\x65\xb8\x91"
                             "\xc2\xa2\x11\x16\x7a\xbb\x8c\x5e\x07\x9e\x09\xe2\xc8\xa8\x33\x9c", 32) == 0) {
    FAIL("TLS 1.3 servers returning HelloRetryRequest are not supprted");
  }
  if (response[43] == '\x00') {
    FAIL("TLS <= 1.2: empty session_id");
  }
  EXPECT_STR(43, "\x20", "Non-TLS response 4");
  if (server_hello_length <= 75) {
    FAIL("Receive too short server hello 2");
  }
  if (memcmp (response + 44, request_session_id, 32) != 0) {
    FAIL("TLS <= 1.2: expected mirrored session_id");
  }
  EXPECT_STR(76, "\x13\x01\x00", "TLS <= 1.2: expected x25519 as a chosen cipher");
  pos += 74;
  int extensions_length = read_length (response, &pos);
  if (extensions_length + 76 != server_hello_length) {
    FAIL("Receive wrong extensions length");
  }
  int sum = 0;
  while (pos < 5 + server_hello_length - 4) {
    int extension_id = read_length (response, &pos);
    if (extension_id != 0x33 && extension_id != 0x2b) {
      FAIL("Receive unexpected extension");
    }
    if (pos == 83) {
      *is_reversed_extension_order = (extension_id == 0x2b);
    }
    sum += extension_id;

    int extension_length = read_length (response, &pos);
    if (pos + extension_length > 5 + server_hello_length) {
      FAIL("Receive wrong extension length");
    }
    if (extension_length != (extension_id == 0x33 ? 36 : 2)) {
      FAIL("Unexpected extension length");
    }
    pos += extension_length;
  }
  if (sum != 0x33 + 0x2b) {
    FAIL("Receive duplicate extensions");
  }
  if (pos != 5 + server_hello_length) {
    FAIL("Receive wrong extensions list");
  }

  CHECK_LENGTH(9);
  EXPECT_STR(pos, "\x14\x03\x03\x00\x01\x01", "Expected dummy ChangeCipherSpec");
  EXPECT_STR(pos + 6, "\x17\x03\x03", "Expected encrypted application data");
  pos += 9;

  CHECK_LENGTH(2);
  *encrypted_application_data_length = read_length (response, &pos);
  if (*encrypted_application_data_length == 0) {
    FAIL("Receive empty encrypted application data");
  }

  CHECK_LENGTH(*encrypted_application_data_length);
  pos += *encrypted_application_data_length;
  if (pos != len) {
    FAIL("Too long");
  }
#undef FAIL
#undef CHECK_LENGTH
#undef EXPECT_STR

  return 1;
}

static int update_domain_info (struct domain_info *info) {
  const char *domain = info->domain;
  struct hostent *host = kdb_gethostbyname (domain);
  if (host == NULL || host->h_addr == NULL) {
    kprintf ("Failed to resolve host %s\n", domain);
    return 0;
  }
  assert (host->h_addrtype == AF_INET || host->h_addrtype == AF_INET6);

  fd_set read_fd;
  fd_set write_fd;
  fd_set except_fd;
  FD_ZERO(&read_fd);
  FD_ZERO(&write_fd);
  FD_ZERO(&except_fd);

#define TRIES 20
  int sockets[TRIES];
  int i;
  for (i = 0; i < TRIES; i++) {
    sockets[i] = socket (host->h_addrtype, SOCK_STREAM, IPPROTO_TCP);
    if (sockets[i] < 0) {
      kprintf ("Failed to open socket for %s: %s\n", domain, strerror (errno));
      return 0;
    }
    if (fcntl (sockets[i], F_SETFL, O_NONBLOCK) == -1) {
      kprintf ("Failed to make socket non-blocking: %s\n", strerror (errno));
      return 0;
    }

    int e_connect;
    if (host->h_addrtype == AF_INET) {
      info->target = *((struct in_addr *) host->h_addr);
      memset (info->target_ipv6, 0, sizeof (info->target_ipv6));

      struct sockaddr_in addr;
      memset (&addr, 0, sizeof (addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons (443);
      memcpy (&addr.sin_addr, host->h_addr, sizeof (struct in_addr));

      e_connect = connect (sockets[i], &addr, sizeof (addr));
    } else {
      assert (sizeof (struct in6_addr) == sizeof (info->target_ipv6));
      info->target.s_addr = 0;
      memcpy (info->target_ipv6, host->h_addr, sizeof (struct in6_addr));

      struct sockaddr_in6 addr;
      memset (&addr, 0, sizeof (addr));
      addr.sin6_family = AF_INET6;
      addr.sin6_port = htons (443);
      memcpy (&addr.sin6_addr, host->h_addr, sizeof (struct in6_addr));

      e_connect = connect (sockets[i], &addr, sizeof (addr));
    }

    if (e_connect == -1 && errno != EINPROGRESS) {
      kprintf ("Failed to connect to %s: %s\n", domain, strerror (errno));
      return 0;
    }
  }

  unsigned char *requests[TRIES];
  for (i = 0; i < TRIES; i++) {
    requests[i] = create_request (domain);
  }
  unsigned char *responses[TRIES] = {};
  int response_len[TRIES] = {};
  int is_encrypted_application_data_length_read[TRIES] = {};

  int finished_count = 0;
  int is_written[TRIES] = {};
  int is_finished[TRIES] = {};
  int read_pos[TRIES] = {};
  double finish_time = get_utime_monotonic() + 5.0;
  int encrypted_application_data_length_min = 0;
  int encrypted_application_data_length_sum = 0;
  int encrypted_application_data_length_max = 0;
  int is_reversed_extension_order_min = 0;
  int is_reversed_extension_order_max = 0;
  int have_error = 0;
  while (get_utime_monotonic() < finish_time && finished_count < TRIES && !have_error) {
    struct timeval timeout_data;
    timeout_data.tv_sec = (int)(finish_time - precise_now + 1);
    timeout_data.tv_usec = 0;

    int max_fd = 0;
    for (i = 0; i < TRIES; i++) {
      if (is_finished[i]) {
        continue;
      }
      if (is_written[i]) {
        FD_SET(sockets[i], &read_fd);
        FD_CLR(sockets[i], &write_fd);
      } else {
        FD_CLR(sockets[i], &read_fd);
        FD_SET(sockets[i], &write_fd);
      }
      FD_SET(sockets[i], &except_fd);
      if (sockets[i] > max_fd) {
        max_fd = sockets[i];
      }
    }

    select (max_fd + 1, &read_fd, &write_fd, &except_fd, &timeout_data);

    for (i = 0; i < TRIES; i++) {
      if (is_finished[i]) {
        continue;
      }
      if (FD_ISSET(sockets[i], &read_fd)) {
        assert (is_written[i]);

        unsigned char header[5];
        if (responses[i] == NULL) {
          ssize_t read_res = read (sockets[i], header, sizeof (header));
          if (read_res != sizeof (header)) {
            kprintf ("Failed to read response header for checking domain %s: %s\n", domain, read_res == -1 ? strerror (errno) : "Read less bytes than expected");
            have_error = 1;
            break;
          }
          if (memcmp (header, "\x16\x03\x03", 3) != 0) {
            kprintf ("Non-TLS response, or TLS <= 1.1, or unsuccessful request to %s: receive \\x%02x\\x%02x\\x%02x\\x%02x\\x%02x...\n",
                     domain, header[0], header[1], header[2], header[3], header[4]);
            have_error = 1;
            break;
          }
          response_len[i] = 5 + header[3] * 256 + header[4] + 6 + 5;
          responses[i] = malloc (response_len[i]);
          memcpy (responses[i], header, sizeof (header));
          read_pos[i] = 5;
        } else {
          ssize_t read_res = read (sockets[i], responses[i] + read_pos[i], response_len[i] - read_pos[i]);
          if (read_res == -1) {
            kprintf ("Failed to read response from %s: %s\n", domain, strerror (errno));
            have_error = 1;
            break;
          }
          read_pos[i] += read_res;

          if (read_pos[i] == response_len[i]) {
            if (!is_encrypted_application_data_length_read[i]) {
              if (memcmp (responses[i] + response_len[i] - 11, "\x14\x03\x03\x00\x01\x01\x17\x03\x03", 9) != 0) {
                kprintf ("Not found TLS 1.3 support on domain %s\n", domain);
                have_error = 1;
                break;
              }

              is_encrypted_application_data_length_read[i] = 1;
              int encrypted_application_data_length = responses[i][response_len[i] - 2] * 256 + responses[i][response_len[i] - 1];
              response_len[i] += encrypted_application_data_length;
              unsigned char *new_buffer = realloc (responses[i], response_len[i]);
              assert (new_buffer != NULL);
              responses[i] = new_buffer;
              continue;
            }

            int is_reversed_extension_order = -1;
            int encrypted_application_data_length = -1;
            if (check_response (responses[i], response_len[i], requests[i] + 44, &is_reversed_extension_order, &encrypted_application_data_length)) {
              assert (is_reversed_extension_order != -1);
              assert (encrypted_application_data_length != -1);
              if (finished_count == 0) {
                is_reversed_extension_order_min = is_reversed_extension_order;
                is_reversed_extension_order_max = is_reversed_extension_order;
                encrypted_application_data_length_min = encrypted_application_data_length;
                encrypted_application_data_length_max = encrypted_application_data_length;
              } else {
                if (is_reversed_extension_order < is_reversed_extension_order_min) {
                  is_reversed_extension_order_min = is_reversed_extension_order;
                }
                if (is_reversed_extension_order > is_reversed_extension_order_max) {
                  is_reversed_extension_order_max = is_reversed_extension_order;
                }
                if (encrypted_application_data_length < encrypted_application_data_length_min) {
                  encrypted_application_data_length_min = encrypted_application_data_length;
                }
                if (encrypted_application_data_length > encrypted_application_data_length_max) {
                  encrypted_application_data_length_max = encrypted_application_data_length;
                }
              }
              encrypted_application_data_length_sum += encrypted_application_data_length;

              FD_CLR(sockets[i], &write_fd);
              FD_CLR(sockets[i], &read_fd);
              FD_CLR(sockets[i], &except_fd);
              is_finished[i] = 1;
              finished_count++;
            } else {
              have_error = 1;
              break;
            }
          }
        }
      }
      if (FD_ISSET(sockets[i], &write_fd)) {
        assert (!is_written[i]);
        ssize_t write_res = write (sockets[i], requests[i], TLS_REQUEST_LENGTH);
        if (write_res != TLS_REQUEST_LENGTH) {
          kprintf ("Failed to write request for checking domain %s: %s", domain, write_res == -1 ? strerror (errno) : "Written less bytes than expected");
          have_error = 1;
          break;
        }
        is_written[i] = 1;
      }
      if (FD_ISSET(sockets[i], &except_fd)) {
        kprintf ("Failed to check domain %s: %s\n", domain, strerror (errno));
        have_error = 1;
        break;
      }
    }
  }

  for (i = 0; i < TRIES; i++) {
    close (sockets[i]);
    free (requests[i]);
    free (responses[i]);
  }

  if (finished_count != TRIES) {
    if (!have_error) {
      kprintf ("Failed to check domain %s in 5 seconds\n", domain);
    }
    return 0;
  }

  if (is_reversed_extension_order_min != is_reversed_extension_order_max) {
    kprintf ("Upstream server %s uses non-deterministic extension order\n", domain);
  }

  info->is_reversed_extension_order = (char)is_reversed_extension_order_min;

  if (encrypted_application_data_length_min == encrypted_application_data_length_max) {
    info->server_hello_encrypted_size = encrypted_application_data_length_min;
    info->use_random_encrypted_size = 0;
  } else if (encrypted_application_data_length_max - encrypted_application_data_length_min <= 3) {
    info->server_hello_encrypted_size = encrypted_application_data_length_max - 1;
    info->use_random_encrypted_size = 1;
  } else {
    kprintf ("Unrecognized encrypted application data length pattern with min = %d, max = %d, mean = %.3lf\n",
             encrypted_application_data_length_min, encrypted_application_data_length_max, encrypted_application_data_length_sum * 1.0 / TRIES);
    info->server_hello_encrypted_size = (int)(encrypted_application_data_length_sum * 1.0 / TRIES + 0.5);
    info->use_random_encrypted_size = 1;
  }

  vkprintf (0, "Successfully checked domain %s in %.3lf seconds: is_reversed_extension_order = %d, server_hello_encrypted_size = %d, use_random_encrypted_size = %d\n",
            domain, get_utime_monotonic() - (finish_time - 5.0), info->is_reversed_extension_order, info->server_hello_encrypted_size, info->use_random_encrypted_size);
  if (info->is_reversed_extension_order && info->server_hello_encrypted_size <= 1250) {
    kprintf ("Multiple encrypted client data packets are unsupported, so handshake with %s will not be fully emulated\n", domain);
  }
  return 1;
#undef TRIES
}

#undef TLS_REQUEST_LENGTH

static const struct domain_info *get_sni_domain_info (const unsigned char *request, int len) {
#define CHECK_LENGTH(length)  \
  if (pos + (length) > len) { \
    return NULL;              \
  }

  int pos = 11 + 32 + 1 + 32;
  CHECK_LENGTH(2);
  int cipher_suites_length = read_length (request, &pos);
  CHECK_LENGTH(cipher_suites_length + 4);
  pos += cipher_suites_length + 4;
  while (1) {
    CHECK_LENGTH(4);
    int extension_id = read_length (request, &pos);
    int extension_length = read_length (request, &pos);
    CHECK_LENGTH(extension_length);

    if (extension_id == 0) {
      // found SNI
      CHECK_LENGTH(5);
      int inner_length = read_length (request, &pos);
      if (inner_length != extension_length - 2) {
        return NULL;
      }
      if (request[pos++] != 0) {
        return NULL;
      }
      int domain_length = read_length (request, &pos);
      if (domain_length != extension_length - 5) {
        return NULL;
      }
      int i;
      for (i = 0; i < domain_length; i++) {
        if (request[pos + i] == 0) {
          return NULL;
        }
      }
      const struct domain_info *info = get_domain_info ((const char *)(request + pos), domain_length);
      if (info == NULL) {
        vkprintf (1, "Receive request for unknown domain %.*s\n", domain_length, request + pos);
      }
      return info;
    }

    pos += extension_length;
  }
#undef CHECK_LENGTH
}

void tcp_rpc_add_proxy_domain (const char *domain) {
  assert (domain != NULL);

  struct domain_info *info = calloc (1, sizeof (struct domain_info));
  assert (info != NULL);
  info->domain = strdup (domain);

  struct domain_info **bucket = get_domain_info_bucket (domain, strlen (domain));
  info->next = *bucket;
  *bucket = info;

  if (!allow_only_tls) {
    allow_only_tls = 1;
    default_domain_info = info;
  }
}

void tcp_rpc_init_proxy_domains() {
  int i;
  for (i = 0; i < DOMAIN_HASH_MOD; i++) {
    struct domain_info *info = domains[i];
    while (info != NULL) {
      if (!update_domain_info (info)) {
        kprintf ("Failed to update response data about %s, so default response settings wiil be used\n", info->domain);
        // keep target addresses as is
        info->is_reversed_extension_order = 0;
        info->use_random_encrypted_size = 1;
        info->server_hello_encrypted_size = 2500 + rand() % 1120;
      }

      info = info->next;
    }
  }
}

struct client_random {
  unsigned char random[16];
  struct client_random *next_by_time;
  struct client_random *next_by_hash;
  int time;
};

#define RANDOM_HASH_BITS 14
static struct client_random *client_randoms[1 << RANDOM_HASH_BITS];

static struct client_random *first_client_random;
static struct client_random *last_client_random;

static struct client_random **get_client_random_bucket (unsigned char random[16]) {
  int i = RANDOM_HASH_BITS;
  int pos = 0;
  int id = 0;
  while (i > 0) {
    int bits = i < 8 ? i : 8;
    id = (id << bits) | (random[pos++] & ((1 << bits) - 1));
    i -= bits;
  }
  assert (0 <= id && id < (1 << RANDOM_HASH_BITS));
  return client_randoms + id;
}

static int have_client_random (unsigned char random[16]) {
  struct client_random *cur = *get_client_random_bucket (random);
  while (cur != NULL) {
    if (memcmp (random, cur->random, 16) == 0) {
      return 1;
    }
    cur = cur->next_by_hash;
  }
  return 0;
}

static void add_client_random (unsigned char random[16]) {
  struct client_random *entry = malloc (sizeof (struct client_random));
  memcpy (entry->random, random, 16);
  entry->time = now;
  entry->next_by_time = NULL;
  if (last_client_random == NULL) {
    assert (first_client_random == NULL);
    first_client_random = last_client_random = entry;
  } else {
    last_client_random->next_by_time = entry;
    last_client_random = entry;
  }

  struct client_random **bucket = get_client_random_bucket (random);
  entry->next_by_hash = *bucket;
  *bucket = entry;
}

#define MAX_CLIENT_RANDOM_CACHE_TIME 2 * 86400

static void delete_old_client_randoms() {
  while (first_client_random != last_client_random) {
    assert (first_client_random != NULL);
    if (first_client_random->time > now - MAX_CLIENT_RANDOM_CACHE_TIME) {
      return;
    }

    struct client_random *entry = first_client_random;
    assert (entry->next_by_hash == NULL);

    first_client_random = first_client_random->next_by_time;

    struct client_random **cur = get_client_random_bucket (entry->random);
    while (*cur != entry) {
      cur = &(*cur)->next_by_hash;
    }
    *cur = NULL;

    free (entry);
  }
}

static int is_allowed_timestamp (int timestamp) {
  if (timestamp > now + 3) {
    // do not allow timestamps in the future
    // after time synchronization client should always have time in the past
    vkprintf (1, "Disallow request with timestamp %d from the future, now is %d\n", timestamp, now);
    return 0;
  }

  // first_client_random->time is an exact time when corresponding request was received
  // if the timestamp is bigger than (first_client_random->time + 3), then the current request could be accepted
  // only after the request with first_client_random, so the client random still must be cached
  // if the request wasn't accepted, then the client_random still will be cached for MAX_CLIENT_RANDOM_CACHE_TIME seconds,
  // so we can miss duplicate request only after a lot of time has passed
  if (first_client_random != NULL && timestamp > first_client_random->time + 3) {
    vkprintf (1, "Allow new request with timestamp %d\n", timestamp);
    return 1;
  }

  // allow all requests with timestamp recently in past, regardless of ability to check repeating client random
  // the allowed error must be big enough to allow requests after time synchronization
  const int MAX_ALLOWED_TIMESTAMP_ERROR = 10 * 60;
  if (timestamp > now - MAX_ALLOWED_TIMESTAMP_ERROR) {
    // this can happen only first (MAX_ALLOWED_TIMESTAMP_ERROR + 3) sceonds after first_client_random->time
    vkprintf (1, "Allow recent request with timestamp %d without full check for client random duplication\n", timestamp);
    return 1;
  }

  // the request is too old to check client random, do not allow it to force client to synchronize it's time
  vkprintf (1, "Disallow too old request with timestamp %d\n", timestamp);
  return 0;
}

static int proxy_connection (connection_job_t C, const struct domain_info *info) {
  struct connection_info *c = CONN_INFO(C);
  assert (check_conn_functions (&ct_proxy_pass, 0) >= 0);

  const char zero[16] = {};
  if (info->target.s_addr == 0 && !memcmp (info->target_ipv6, zero, 16)) {
    vkprintf (0, "failed to proxy request to %s\n", info->domain);
    fail_connection (C, -17);
    return 0;
  }

  int port = c->our_port == 80 ? 80 : 443;

  int cfd = -1;
  if (info->target.s_addr) {
    cfd = client_socket (info->target.s_addr, port, 0);
  } else {
    cfd = client_socket_ipv6 (info->target_ipv6, port, SM_IPV6);
  }

  if (cfd < 0) {
    kprintf ("failed to create proxy pass connection: %d (%m)", errno);
    fail_connection (C, -27);
    return 0;
  }

  c->type->crypto_free (C);
  job_incref (C); 
  job_t EJ = alloc_new_connection (cfd, NULL, NULL, ct_outbound, &ct_proxy_pass, C, ntohl (*(int *)&info->target.s_addr), (void *)info->target_ipv6, port); 

  if (!EJ) {
    kprintf ("failed to create proxy pass connection (2)");
    job_decref_f (C);
    fail_connection (C, -37);
    return 0;
  }

  c->type = &ct_proxy_pass;
  c->extra = job_incref (EJ);
      
  assert (CONN_INFO(EJ)->io_conn);
  unlock_job (JOB_REF_PASS (EJ));

  return c->type->parse_execute (C);
}

int tcp_rpcs_ext_alarm (connection_job_t C) {
  struct tcp_rpc_data *D = TCP_RPC_DATA (C);
  if (D->in_packet_num == -3 && default_domain_info != NULL) {
    return proxy_connection (C, default_domain_info);  
  } else {
    return 0;
  }
}

int tcp_rpcs_ext_init_accepted (connection_job_t C) {
  job_timer_insert (C, precise_now + 10);
  return tcp_rpcs_init_accepted_nohs (C);
}

int tcp_rpcs_compact_parse_execute (connection_job_t C) {
#define RETURN_TLS_ERROR(info) \
  return proxy_connection (C, info);  

  struct tcp_rpc_data *D = TCP_RPC_DATA (C);
  if (D->crypto_flags & RPCF_COMPACT_OFF) {
    if (D->in_packet_num != -3) {
      job_timer_remove (C);
    }
    return tcp_rpcs_parse_execute (C);
  }

  struct connection_info *c = CONN_INFO (C);
  int len;

  vkprintf (4, "%s. in_total_bytes = %d\n", __func__, c->in.total_bytes);

  while (1) {
    if (D->in_packet_num != -3) {
      job_timer_remove (C);
    }
    if (c->flags & C_ERROR) {
      return NEED_MORE_BYTES;
    }
    if (c->flags & C_STOPPARSE) {
      return NEED_MORE_BYTES;
    }
    len = c->in.total_bytes; 
    if (len <= 0) {
      return NEED_MORE_BYTES;
    }

    int min_len = (D->flags & RPC_F_MEDIUM) ? 4 : 1;
    if (len < min_len + 8) {
      return min_len + 8 - len;
    }

    int packet_len = 0;
    assert (rwm_fetch_lookup (&c->in, &packet_len, 4) == 4);

    if (D->in_packet_num == -3) {
      vkprintf (1, "trying to determine type of connection from %s:%d\n", show_remote_ip (C), c->remote_port);
#if __ALLOW_UNOBFS__
      if ((packet_len & 0xff) == 0xef) {
        D->flags |= RPC_F_COMPACT;
        assert (rwm_skip_data (&c->in, 1) == 1);
        D->in_packet_num = 0;
        vkprintf (1, "Short type\n");
        continue;
      } 
      if (packet_len == 0xeeeeeeee) {
        D->flags |= RPC_F_MEDIUM;
        assert (rwm_skip_data (&c->in, 4) == 4);
        D->in_packet_num = 0;
        vkprintf (1, "Medium type\n");
        continue;
      }
      if (packet_len == 0xdddddddd) {
        D->flags |= RPC_F_MEDIUM | RPC_F_PAD;
        assert (rwm_skip_data (&c->in, 4) == 4);
        D->in_packet_num = 0;
        vkprintf (1, "Medium type\n");
        continue;
      }
        
      // http
      if ((packet_len == *(int *)"HEAD" || packet_len == *(int *)"POST" || packet_len == *(int *)"GET " || packet_len == *(int *)"OPTI") && TCP_RPCS_FUNC(C)->http_fallback_type) {
        D->crypto_flags |= RPCF_COMPACT_OFF;
        vkprintf (1, "HTTP type\n");
        return tcp_rpcs_parse_execute (C);
      }
#endif

      // fake tls
      if (c->flags & C_IS_TLS) {
        if (len < 11) {
          return 11 - len;
        }

        vkprintf (1, "Established TLS connection from %s:%d\n", show_remote_ip (C), c->remote_port);
        unsigned char header[11];
        assert (rwm_fetch_lookup (&c->in, header, 11) == 11);
        if (memcmp (header, "\x14\x03\x03\x00\x01\x01\x17\x03\x03", 9) != 0) {
          vkprintf (1, "error while parsing packet: bad client dummy ChangeCipherSpec\n");
          fail_connection (C, -1);
          return 0;
        }

        min_len = 11 + 256 * header[9] + header[10];
        if (len < min_len) {
          vkprintf (2, "Need %d bytes, but have only %d\n", min_len, len);
          return min_len - len;
        }

        assert (rwm_skip_data (&c->in, 11) == 11);
        len -= 11;
        c->left_tls_packet_length = 256 * header[9] + header[10]; // store left length of current TLS packet in extra_int3
        vkprintf (2, "Receive first TLS packet of length %d\n", c->left_tls_packet_length);

        if (c->left_tls_packet_length < 64) {
          vkprintf (1, "error while parsing packet: too short first TLS packet: %d\n", c->left_tls_packet_length);
          fail_connection (C, -1);
          return 0;
        }
        // now len >= c->left_tls_packet_length >= 64

        assert (rwm_fetch_lookup (&c->in, &packet_len, 4) == 4);

        c->left_tls_packet_length -= 64; // skip header length
      } else if ((packet_len & 0xFFFFFF) == 0x010316 && (packet_len >> 24) >= 2 && tcp_rpcs_active_secret_count (ext_now_sec()) > 0 && allow_only_tls) {
        unsigned char header[5];
        assert (rwm_fetch_lookup (&c->in, header, 5) == 5);
        min_len = 5 + 256 * header[3] + header[4];
        if (len < min_len) {
          return min_len - len;
        }

        int read_len = len <= 4096 ? len : 4096;
        unsigned char client_hello[read_len + 1]; // VLA
        assert (rwm_fetch_lookup (&c->in, client_hello, read_len) == read_len);

        const struct domain_info *info = get_sni_domain_info (client_hello, read_len);
        if (info == NULL) {
          RETURN_TLS_ERROR(default_domain_info);
        }

        vkprintf (1, "TLS type with domain %s from %s:%d\n", info->domain, show_remote_ip (C), c->remote_port);

        if (c->our_port == 80) {
          vkprintf (1, "Receive TLS request on port %d, proxying to %s\n", c->our_port, info->domain);
          RETURN_TLS_ERROR(info);
        }

        if (len > min_len) {
          vkprintf (1, "Too much data in ClientHello, receive %d instead of %d\n", len, min_len);
          RETURN_TLS_ERROR(info);
        }
        if (len != read_len) {
          vkprintf (1, "Too big ClientHello: receive %d bytes\n", len);
          RETURN_TLS_ERROR(info);
        }

        unsigned char client_random[32];
        memcpy (client_random, client_hello + 11, 32);
        memset (client_hello + 11, '\0', 32);

        if (have_client_random (client_random)) {
          vkprintf (1, "Receive again request with the same client random\n");
          RETURN_TLS_ERROR(info);
        }
        add_client_random (client_random);
        delete_old_client_randoms();

        unsigned char expected_random[32];
        unsigned char matched_secret[16];
        int have_match = 0;

        unsigned char *active_secrets = NULL;
        int active_secret_cnt = 0;
        if (ext_snapshot_active_secrets (ext_now_sec(), &active_secrets, &active_secret_cnt) < 0) {
          RETURN_TLS_ERROR(info);
        }
        int secret_id;
        for (secret_id = 0; secret_id < active_secret_cnt; secret_id++) {
          unsigned char *secret = active_secrets + 16 * secret_id;
          sha256_hmac (secret, 16, client_hello, len, expected_random);
          if (memcmp (expected_random, client_random, 28) == 0) {
            memcpy (matched_secret, secret, 16);
            have_match = 1;
            break;
          }
        }
        free (active_secrets);

        if (!have_match) {
          vkprintf (1, "Receive request with unmatched client random\n");
          RETURN_TLS_ERROR(info);
        }
        int timestamp = *(int *)(expected_random + 28) ^ *(int *)(client_random + 28);
        if (!is_allowed_timestamp (timestamp)) {
          RETURN_TLS_ERROR(info);
        }

        int pos = 76;
        int cipher_suites_length = read_length (client_hello, &pos);
        if (pos + cipher_suites_length > read_len) {
          vkprintf (1, "Too long cipher suites list of length %d\n", cipher_suites_length);
          RETURN_TLS_ERROR(info);
        }
        while (cipher_suites_length >= 2 && (client_hello[pos] & 0x0F) == 0x0A && (client_hello[pos + 1] & 0x0F) == 0x0A) {
          // skip grease
          cipher_suites_length -= 2;
          pos += 2;
        }
        if (cipher_suites_length <= 1 || client_hello[pos] != 0x13 || client_hello[pos + 1] < 0x01 || client_hello[pos + 1] > 0x03) {
          vkprintf (1, "Can't find supported cipher suite\n");
          RETURN_TLS_ERROR(info);
        }
        unsigned char cipher_suite_id = client_hello[pos + 1];

        assert (rwm_skip_data (&c->in, len) == len);
        c->flags |= C_IS_TLS;
        c->left_tls_packet_length = -1;

        int encrypted_size = get_domain_server_hello_encrypted_size (info);
        int response_size = 127 + 6 + 5 + encrypted_size;
        unsigned char *buffer = malloc (32 + response_size);
        assert (buffer != NULL);
        memcpy (buffer, client_random, 32);
        unsigned char *response_buffer = buffer + 32;
        memcpy (response_buffer, "\x16\x03\x03\x00\x7a\x02\x00\x00\x76\x03\x03", 11);
        memset (response_buffer + 11, '\0', 32);
        response_buffer[43] = '\x20';
        memcpy (response_buffer + 44, client_hello + 44, 32);
        memcpy (response_buffer + 76, "\x13\x01\x00\x00\x2e", 5);
        response_buffer[77] = cipher_suite_id;

        pos = 81;
        int tls_server_extensions[3] = {0x33, 0x2b, -1};
        if (info->is_reversed_extension_order) {
          int t = tls_server_extensions[0];
          tls_server_extensions[0] = tls_server_extensions[1];
          tls_server_extensions[1] = t;
        }
        int i;
        for (i = 0; tls_server_extensions[i] != -1; i++) {
          if (tls_server_extensions[i] == 0x33) {
            assert (pos + 40 <= response_size);
            memcpy (response_buffer + pos, "\x00\x33\x00\x24\x00\x1d\x00\x20", 8);
            generate_public_key (response_buffer + pos + 8);
            pos += 40;
          } else if (tls_server_extensions[i] == 0x2b) {
            assert (pos + 5 <= response_size);
            memcpy (response_buffer + pos, "\x00\x2b\x00\x02\x03\x04", 6);
            pos += 6;
          } else {
            assert (0);
          }
        }
        assert (pos == 127);
        memcpy (response_buffer + 127, "\x14\x03\x03\x00\x01\x01\x17\x03\x03", 9);
        pos += 9;
        response_buffer[pos++] = encrypted_size / 256;
        response_buffer[pos++] = encrypted_size % 256;
        assert (pos + encrypted_size == response_size);
        RAND_bytes (response_buffer + pos, encrypted_size);

        unsigned char server_random[32];
        sha256_hmac (matched_secret, 16, buffer, 32 + response_size, server_random);
        memcpy (response_buffer + 11, server_random, 32);

        struct raw_message *m = calloc (sizeof (struct raw_message), 1);
        rwm_create (m, response_buffer, response_size);
        mpq_push_w (c->out_queue, m, 0);
        job_signal (JOB_REF_CREATE_PASS (C), JS_RUN);

        free (buffer);
        return 11; // waiting for dummy ChangeCipherSpec and first packet
      }

      if (allow_only_tls && !(c->flags & C_IS_TLS)) {
        vkprintf (1, "Expected TLS-transport\n");
        RETURN_TLS_ERROR(default_domain_info);
      }

#if __ALLOW_UNOBFS__
      int tmp[2];
      assert (rwm_fetch_lookup (&c->in, &tmp, 8) == 8);
      if (!tmp[1] && !(c->flags & C_IS_TLS)) {
        D->crypto_flags |= RPCF_COMPACT_OFF;
        vkprintf (1, "Long type\n");
        return tcp_rpcs_parse_execute (C);
      }
#endif

      if (len < 64) {
        assert (!(c->flags & C_IS_TLS));
#if __ALLOW_UNOBFS__
        vkprintf (1, "random 64-byte header: first 0x%08x 0x%08x, need %d more bytes to distinguish\n", tmp[0], tmp[1], 64 - len);
#else
        vkprintf (1, "\"random\" 64-byte header: have %d bytes, need %d more bytes to distinguish\n", len, 64 - len);
#endif
        return 64 - len;
      }

      unsigned char random_header[64];
      unsigned char k[48];
      assert (rwm_fetch_lookup (&c->in, random_header, 64) == 64);
        
      unsigned char random_header_sav[64];
      memcpy (random_header_sav, random_header, 64);
      
      struct aes_key_data key_data;
      
      int ok = 0;
      int have_active_secrets = 0;
      unsigned char *active_secrets = NULL;
      int active_secret_cnt = 0;
      if (ext_snapshot_active_secrets (ext_now_sec(), &active_secrets, &active_secret_cnt) < 0) {
        return (-1 << 28);
      }
      have_active_secrets = active_secret_cnt > 0;

      int secret_id;
      int attempts = have_active_secrets ? active_secret_cnt : 1;
      for (secret_id = 0; secret_id < attempts; secret_id++) {
        if (have_active_secrets) {
          memcpy (k, random_header + 8, 32);
          memcpy (k + 32, active_secrets + 16 * secret_id, 16);
          sha256 (k, 48, key_data.read_key);
        } else {
          memcpy (key_data.read_key, random_header + 8, 32);
        }
        memcpy (key_data.read_iv, random_header + 40, 16);

        int i;
        for (i = 0; i < 32; i++) {
          key_data.write_key[i] = random_header[55 - i];
        }
        for (i = 0; i < 16; i++) {
          key_data.write_iv[i] = random_header[23 - i];
        }

        if (have_active_secrets) {
          memcpy (k, key_data.write_key, 32);
          sha256 (k, 48, key_data.write_key);
        }

        aes_crypto_ctr128_init (C, &key_data, sizeof (key_data));
        assert (c->crypto);
        struct aes_crypto *T = c->crypto;

        evp_crypt (T->read_aeskey, random_header, random_header, 64);
        unsigned tag = *(unsigned *)(random_header + 56);

        if (tag == 0xdddddddd || tag == 0xeeeeeeee || tag == 0xefefefef) {
          if (tag != 0xdddddddd && allow_only_tls) {
            vkprintf (1, "Expected random padding mode\n");
            RETURN_TLS_ERROR(default_domain_info);
          }
          assert (rwm_skip_data (&c->in, 64) == 64);
          rwm_union (&c->in_u, &c->in);
          rwm_init (&c->in, 0);
          // T->read_pos = 64;
          D->in_packet_num = 0;
          switch (tag) {
            case 0xeeeeeeee:
              D->flags |= RPC_F_MEDIUM | RPC_F_EXTMODE2;
              break;
            case 0xdddddddd:
              D->flags |= RPC_F_MEDIUM | RPC_F_EXTMODE2 | RPC_F_PAD;
              break;
            case 0xefefefef:
              D->flags |= RPC_F_COMPACT | RPC_F_EXTMODE2;
              break;
          }
          assert (c->type->crypto_decrypt_input (C) >= 0);

          int target = *(short *)(random_header + 60);
          D->extra_int4 = target;
          vkprintf (1, "tcp opportunistic encryption mode detected, tag = %08x, target=%d\n", tag, target);
          ok = 1;
          break;
        } else {
          aes_crypto_free (C);
          memcpy (random_header, random_header_sav, 64);
        }
      }

      if (ok) {
        free (active_secrets);
        continue;
      }

      free (active_secrets);

      if (have_active_secrets) {
        vkprintf (1, "invalid \"random\" 64-byte header, entering global skip mode\n");
        return (-1 << 28);
      }

#if __ALLOW_UNOBFS__
      vkprintf (1, "short type with 64-byte header: first 0x%08x 0x%08x\n", tmp[0], tmp[1]);
      D->flags |= RPC_F_COMPACT | RPC_F_EXTMODE1;
      D->in_packet_num = 0;

      assert (len >= 64);
      assert (rwm_skip_data (&c->in, 64) == 64);
      continue;
#else
      vkprintf (1, "invalid \"random\" 64-byte header, entering global skip mode\n");
      return (-1 << 28);
#endif
    }

    int packet_len_bytes = 4;
    if (D->flags & RPC_F_MEDIUM) {
      // packet len in `medium` mode
      //if (D->crypto_flags & RPCF_QUICKACK) {
        D->flags = (D->flags & ~RPC_F_QUICKACK) | (packet_len & RPC_F_QUICKACK);
        packet_len &= ~RPC_F_QUICKACK;
      //}
    } else {
      // packet len in `compact` mode
      if (packet_len & 0x80) {
        D->flags |= RPC_F_QUICKACK;
        packet_len &= ~0x80;
      } else {
        D->flags &= ~RPC_F_QUICKACK;
      }
      if ((packet_len & 0xff) == 0x7f) {
        packet_len = ((unsigned) packet_len >> 8);
        if (packet_len < 0x7f) {
          vkprintf (1, "error while parsing compact packet: got length %d in overlong encoding\n", packet_len);
          fail_connection (C, -1);
          return 0;
        }
      } else {
        packet_len &= 0x7f;
        packet_len_bytes = 1;
      }
      packet_len <<= 2;
    }

    if (packet_len <= 0 || (packet_len & 0xc0000000) || (!(D->flags & RPC_F_PAD) && (packet_len & 3))) {
      vkprintf (1, "error while parsing packet: bad packet length %d\n", packet_len);
      fail_connection (C, -1);
      return 0;
    }

    if ((packet_len > TCP_RPCS_FUNC(C)->max_packet_len && TCP_RPCS_FUNC(C)->max_packet_len > 0))  {
      vkprintf (1, "error while parsing packet: bad packet length %d\n", packet_len);
      fail_connection (C, -1);
      return 0;
    }

    if (len < packet_len + packet_len_bytes) {
      return packet_len + packet_len_bytes - len;
    }

    assert (rwm_skip_data (&c->in, packet_len_bytes) == packet_len_bytes);
    
    struct raw_message msg;
    int packet_type;

    rwm_split_head (&msg, &c->in, packet_len);
    if (D->flags & RPC_F_PAD) {
      rwm_trunc (&msg, packet_len & -4);
    }

    assert (rwm_fetch_lookup (&msg, &packet_type, 4) == 4);

    if (D->in_packet_num < 0) {
      assert (D->in_packet_num == -3);
      D->in_packet_num = 0;
    }

    if (verbosity > 2) {
      kprintf ("received packet from connection %d (length %d, num %d, type %08x)\n", c->fd, packet_len, D->in_packet_num, packet_type);
      rwm_dump (&msg);
    }

    int res = -1;

    /* main case */
    c->last_response_time = precise_now;
    if (packet_type == RPC_PING) {
      res = tcp_rpcs_default_execute (C, packet_type, &msg);
    } else {
      res = TCP_RPCS_FUNC(C)->execute (C, packet_type, &msg);
    }
    if (res <= 0) {
      rwm_free (&msg);
    }

    D->in_packet_num++;
  }
  return NEED_MORE_BYTES;
#undef RETURN_TLS_ERROR
}

/*
 *
 *                END (EXTERNAL RPC SERVER)
 *
 */
