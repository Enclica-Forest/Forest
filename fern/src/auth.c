#include "include/auth.h"
#include "include/string.h"
#include "include/memory.h"
#include "include/debuglog.h"
#include "include/vfs.h"
#include "include/util.h"
#include "include/timer.h"

#define AUTH_PASS_MAX 64

// Hash format version tag, kept internal to auth.c. AUTH_HASH_HEX_LEN is a
// fixed 65-byte (64 hex chars + NUL) buffer with no spare room for an
// in-band marker, so the format is tracked out-of-band per record instead.
//   AUTH_HASH_VERSION_LEGACY   - hash_hex is a single-round SHA256(salt||':'||password)
//                                 digest. Produced by records that were loaded from
//                                 /etc/shadow at boot (import_user_from_shadow), since
//                                 the on-disk shadow format predates the iterated
//                                 construction and carries no version marker of its own.
//   AUTH_HASH_VERSION_ITERATED - hash_hex is the output of the iterated, key-stretched
//                                 hash_password(). Produced by add_user_internal() and
//                                 auth_change_password(), and lazily assigned to a
//                                 record the first time a legacy password verifies
//                                 successfully in auth_login() (rehash-on-next-login).
#define AUTH_HASH_VERSION_LEGACY   0u
#define AUTH_HASH_VERSION_ITERATED 1u

typedef struct {
    auth_user_info_t info;
    char salt[AUTH_SALT_LEN];
    char hash_hex[AUTH_HASH_HEX_LEN];
    uint8 hash_version; // AUTH_HASH_VERSION_LEGACY or AUTH_HASH_VERSION_ITERATED
    bool used;
} user_record_t;

typedef struct {
    auth_group_info_t info;
    bool used;
} group_record_t;

static user_record_t g_users[AUTH_MAX_USERS];
static group_record_t g_groups[AUTH_MAX_GROUPS];
static user_record_t* g_active_user = 0;
static uint32 g_next_uid = 1;
static uint32 g_next_gid = 0;

static const char* DEFAULT_ROOT_PASSWORD = "root";

// ---------------------------------------------------------------------------
// Minimal SHA-256 implementation (for salted password hashing)
// ---------------------------------------------------------------------------
typedef struct {
    uint8 data[64];
    uint32 datalen;
    uint64 bitlen;
    uint32 state[8];
} sha256_ctx_t;

static const uint32 k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32 rotr(uint32 x, uint32 n) { return (x >> n) | (x << (32 - n)); }
static uint32 ch(uint32 x, uint32 y, uint32 z) { return (x & y) ^ ((~x) & z); }
static uint32 maj(uint32 x, uint32 y, uint32 z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint32 bsig0(uint32 x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
static uint32 bsig1(uint32 x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
static uint32 ssig0(uint32 x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
static uint32 ssig1(uint32 x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

static void sha256_transform(sha256_ctx_t* ctx, const uint8* data) {
    uint32 m[64];
    for (uint32 i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    }
    for (uint32 i = 16; i < 64; ++i) {
        m[i] = ssig1(m[i - 2]) + m[i - 7] + ssig0(m[i - 15]) + m[i - 16];
    }

    uint32 a = ctx->state[0];
    uint32 b = ctx->state[1];
    uint32 c = ctx->state[2];
    uint32 d = ctx->state[3];
    uint32 e = ctx->state[4];
    uint32 f = ctx->state[5];
    uint32 g = ctx->state[6];
    uint32 h = ctx->state[7];

    for (uint32 i = 0; i < 64; ++i) {
        uint32 t1 = h + bsig1(e) + ch(e, f, g) + k[i] + m[i];
        uint32 t2 = bsig0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t* ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(sha256_ctx_t* ctx, const uint8* data, uint32 len) {
    for (uint32 i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t* ctx, uint8* hash) {
    uint32 i = ctx->datalen;

    // Pad the remaining data.
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) {
            ctx->data[i++] = 0x00;
        }
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) {
            ctx->data[i++] = 0x00;
        }
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    // Append length in bits.
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = (uint8)(ctx->bitlen);
    ctx->data[62] = (uint8)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    // Produce output hash.
    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
    }
}

static void hex_encode(const uint8* in, uint32 len, char* out, uint32 out_len) {
    static const char* hex = "0123456789abcdef";
    uint32 j = 0;
    for (uint32 i = 0; i < len && j + 1 < out_len; ++i) {
        out[j++] = hex[(in[i] >> 4) & 0xF];
        if (j >= out_len - 1) break;
        out[j++] = hex[in[i] & 0xF];
    }
    out[j] = '\0';
}

// ---------------------------------------------------------------------------
// Helper routines
// ---------------------------------------------------------------------------

static void safe_copy(char* dest, size_t dest_size, const char* src) {
    if (!dest || dest_size == 0) {
        return;
    }
    if (!src) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static uint32 parse_uint(const char* text, uint32 fallback) {
    if (!text) {
        return fallback;
    }
    uint32 value = 0;
    while (*text) {
        if (*text < '0' || *text > '9') {
            break;
        }
        value = value * 10 + (uint32)(*text - '0');
        text++;
    }
    return value;
}

// NOTE on entropy (accepted limitation): this freestanding kernel has no
// hardware RNG / RDRAND / RDSEED usage wired up yet and no OS-level entropy
// pool, so the xorshift PRNG below is seeded only from timer_get_ticks()
// XOR'd with the uid/gid allocation counters. That is low entropy and is
// predictable to an attacker who can observe or influence boot timing and
// user-creation order. The salt's job here is solely to defeat precomputed
// (rainbow-table) attacks across accounts/installs, not to serve as a
// secret; per-account brute-force cost still comes from AUTH_HASH_ROUNDS in
// hash_password(). Mixing in a hardware cycle counter (e.g. RDTSC on x86)
// would raise the bar further and is a reasonable follow-up, but is not a
// blocker for the hashing hardening implemented here.
static void auth_generate_salt(char* out) {
    // g_salt_call_counter guarantees a unique seed component on every call,
    // even for repeated calls against the same account within a single PIT
    // tick (where timer_get_ticks() and the uid/gid counters would otherwise
    // be bit-for-bit identical and produce a colliding salt). It only needs
    // to differ call-to-call, not be secret or random, so a plain
    // monotonically-incrementing counter is sufficient here.
    static uint32 g_salt_call_counter = 0;
    g_salt_call_counter++;
    uint32 seed = timer_get_ticks() ^ (g_next_uid << 8) ^ (g_next_gid << 4) ^ g_salt_call_counter;
    for (uint32 i = 0; i + 1 < AUTH_SALT_LEN; ++i) {
        seed ^= (seed << 13);
        seed ^= (seed >> 17);
        seed ^= (seed << 5);
        out[i] = (char)('a' + (seed % 26));
    }
    out[AUTH_SALT_LEN - 1] = '\0';
}

// Number of extra SHA-256 rounds applied on top of the initial salted digest.
// This is what makes hash_password() an iterated (key-stretching) construction
// instead of a single unsalted-cost SHA-256 call: it multiplies the CPU cost
// of an offline brute-force / dictionary attack against a stolen hash by
// roughly AUTH_HASH_ROUNDS, while leaving the on-disk representation (a
// AUTH_HASH_HEX_LEN-1 character hex string) completely unchanged.
// 200000 rounds of plain SHA-256 (~18 bits of extra work factor vs. a single
// round) roughly costs a few tens of milliseconds on a single CPU core at
// login time -- acceptable for an interactive hobby-OS login path -- while
// being materially more expensive for offline/GPU brute force than the
// previous 20000 rounds.
#define AUTH_HASH_ROUNDS 200000u

// hash_password() derives the AUTH_HASH_HEX_LEN-1 char hex digest stored in
// user_record_t::hash_hex from a salt and a password using the new,
// iterated (key-stretched) construction:
//
//   1. digest_0 = SHA256(salt || ':' || password)
//   2. digest_i = SHA256(digest_(i-1) || salt || password)  for i in [1, AUTH_HASH_ROUNDS]
//   3. out_hex  = hex(digest_AUTH_HASH_ROUNDS)
//
// AUTH_HASH_HEX_LEN is untouched (still a plain 64-hex-char + NUL digest),
// so there is no room for an in-band format marker inside hash_hex itself.
// Records instead carry an out-of-band user_record_t::hash_version tag
// (AUTH_HASH_VERSION_LEGACY / AUTH_HASH_VERSION_ITERATED, both local to this
// file) so that pre-existing /etc/shadow rows -- which were produced by the
// old single-round scheme and have no marker of their own -- keep verifying
// via hash_password_legacy() below instead of being locked out. Every
// call site that *creates* a fresh credential (add_user_internal,
// auth_change_password) always uses this iterated function and tags the
// record AUTH_HASH_VERSION_ITERATED; only records imported from
// /etc/shadow start out tagged AUTH_HASH_VERSION_LEGACY, and get lazily
// upgraded to the iterated format the first time they log in successfully
// (see auth_login()).
static void hash_password(const char* salt, const char* password, char out_hex[AUTH_HASH_HEX_LEN]) {
    // Both the initial digest below and the iterated rounds further down
    // must clamp salt/password to the exact same length -- otherwise two
    // different passwords that share the same leading AUTH_PASS_MAX-1 bytes
    // but differ further on could feed identical bytes into one stage while
    // still differing (or vice versa) in the other, and truncation would
    // silently vary by stage instead of being one well-defined contract.
    size_t salt_len = salt ? strlen(salt) : 0;
    if (salt_len > AUTH_SALT_LEN - 1) {
        salt_len = AUTH_SALT_LEN - 1;
    }
    size_t pass_len = password ? strlen(password) : 0;
    if (pass_len > AUTH_PASS_MAX - 1) {
        pass_len = AUTH_PASS_MAX - 1;
    }

    char combined[AUTH_SALT_LEN + AUTH_PASS_MAX + 4];
    size_t len = 0;
    if (salt_len) {
        memcpy(combined, salt, salt_len);
        len = salt_len;
    }
    combined[len++] = ':';
    if (pass_len) {
        memcpy(combined + len, password, pass_len);
        len += pass_len;
    }

    sha256_ctx_t ctx;
    uint8 digest[32];
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8*)combined, (uint32)len);
    sha256_final(&ctx, digest);

    // Iterated stretching: feed (prior digest || salt || password) back
    // through SHA-256 thousands of times, using the exact same salt_len/
    // pass_len clamp computed above so this stage can never see a longer
    // (or differently truncated) password than the initial digest did.
    uint8 round_buf[sizeof(digest) + (AUTH_SALT_LEN - 1) + (AUTH_PASS_MAX - 1)];
    for (uint32 round = 0; round < AUTH_HASH_ROUNDS; ++round) {
        size_t off = sizeof(digest);
        memcpy(round_buf, digest, sizeof(digest));
        if (salt_len) {
            memcpy(round_buf + off, salt, salt_len);
            off += salt_len;
        }
        if (pass_len) {
            memcpy(round_buf + off, password, pass_len);
            off += pass_len;
        }
        sha256_init(&ctx);
        sha256_update(&ctx, round_buf, (uint32)off);
        sha256_final(&ctx, digest);
    }

    hex_encode(digest, sizeof(digest), out_hex, AUTH_HASH_HEX_LEN);
}

// hash_password_legacy() reproduces the *original*, pre-hardening single
// round construction: out_hex = hex(SHA256(salt || ':' || password)). It
// exists only so that AUTH_HASH_VERSION_LEGACY records loaded from
// /etc/shadow (which were hashed this way before AUTH_HASH_ROUNDS existed)
// can still be verified in auth_login(); it is never used to create new
// credentials. Kept byte-for-byte equivalent to the old hash_password() so
// existing on-disk hashes keep matching.
static void hash_password_legacy(const char* salt, const char* password, char out_hex[AUTH_HASH_HEX_LEN]) {
    char combined[AUTH_SALT_LEN + AUTH_PASS_MAX + 4];
    combined[0] = '\0';
    safe_copy(combined, sizeof(combined), salt);
    size_t len = strlen(combined);
    if (len + 1 < sizeof(combined)) {
        combined[len++] = ':';
        combined[len] = '\0';
    }
    if (password) {
        strncpy(combined + len, password, sizeof(combined) - len - 1);
        combined[sizeof(combined) - 1] = '\0';
    }

    sha256_ctx_t ctx;
    uint8 digest[32];
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8*)combined, (uint32)strlen(combined));
    sha256_final(&ctx, digest);

    hex_encode(digest, sizeof(digest), out_hex, AUTH_HASH_HEX_LEN);
}

// Constant-time comparison of two AUTH_HASH_HEX_LEN-1 char hex hash strings.
// A plain strcmp() short-circuits on the first mismatching byte, so the time
// it takes leaks how many leading characters of an attacker's guess matched
// the stored hash -- an oracle an attacker can use to recover the hash
// character-by-character. Instead this XORs every corresponding byte pair
// and OR-accumulates the result over the full fixed length, walking off the
// end of neither string early, so the runtime does not depend on where (or
// whether) the strings first differ.
static bool hash_hex_equal(const char* a, const char* b) {
    uint8 diff = 0;
    for (uint32 i = 0; i < AUTH_HASH_HEX_LEN - 1; ++i) {
        diff |= (uint8)a[i] ^ (uint8)b[i];
    }
    return diff == 0;
}

static int find_group_slot(const char* name) {
    for (int i = 0; i < AUTH_MAX_GROUPS; ++i) {
        if (g_groups[i].used && strcmp(g_groups[i].info.name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int allocate_group(const char* name) {
    for (int i = 0; i < AUTH_MAX_GROUPS; ++i) {
        if (!g_groups[i].used) {
            g_groups[i].used = true;
            safe_copy(g_groups[i].info.name, sizeof(g_groups[i].info.name), name);
            g_groups[i].info.gid = g_next_gid++;
            return i;
        }
    }
    return -1;
}

static int ensure_group(const char* name) {
    int slot = find_group_slot(name);
    if (slot >= 0) {
        return slot;
    }
    return allocate_group(name);
}

static user_record_t* find_user_record(const char* username) {
    if (!username) {
        return 0;
    }
    for (int i = 0; i < AUTH_MAX_USERS; ++i) {
        if (g_users[i].used && strcmp(g_users[i].info.name, username) == 0) {
            return &g_users[i];
        }
    }
    return 0;
}

static auth_result_t add_user_internal(const char* username, const char* password, uint32 gid, uint32 flags) {
    if (!username || !password) {
        return AUTH_ERR_INVALID;
    }
    if (find_user_record(username)) {
        return AUTH_ERR_EXISTS;
    }

    int free_idx = -1;
    for (int i = 0; i < AUTH_MAX_USERS; ++i) {
        if (!g_users[i].used) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        return AUTH_ERR_FULL;
    }

    user_record_t* rec = &g_users[free_idx];
    memset(rec, 0, sizeof(user_record_t));
    rec->used = true;
    safe_copy(rec->info.name, sizeof(rec->info.name), username);
    rec->info.uid = (flags & AUTH_FLAG_ROOT) ? 0 : g_next_uid++;
    rec->info.gid = gid;
    rec->info.groups_mask = (1u << gid);
    if (flags & AUTH_FLAG_ROOT) {
        rec->info.groups_mask |= AUTH_FLAG_ROOT;
    }
    rec->info.flags = flags;
    auth_generate_salt(rec->salt);
    hash_password(rec->salt, password, rec->hash_hex);
    rec->hash_version = AUTH_HASH_VERSION_ITERATED;
    debuglog(DEBUG_INFO, "[AUTH] Created user '%s' uid=%u gid=%u\n", rec->info.name, rec->info.uid, rec->info.gid);
    return AUTH_OK;
}

static auth_result_t import_user_from_shadow(const char* username,
                                             const char* salt,
                                             const char* hash,
                                             uint32 uid,
                                             uint32 gid,
                                             uint32 group_mask,
                                             uint32 flags) {
    if (!username || !salt || !hash) {
        return AUTH_ERR_INVALID;
    }
    int free_idx = -1;
    for (int i = 0; i < AUTH_MAX_USERS; ++i) {
        if (!g_users[i].used) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        return AUTH_ERR_FULL;
    }
    user_record_t* rec = &g_users[free_idx];
    memset(rec, 0, sizeof(user_record_t));
    rec->used = true;
    safe_copy(rec->info.name, sizeof(rec->info.name), username);
    rec->info.uid = uid;
    rec->info.gid = gid;
    rec->info.groups_mask = group_mask ? group_mask : (1u << gid);
    rec->info.flags = flags;
    safe_copy(rec->salt, sizeof(rec->salt), salt);
    safe_copy(rec->hash_hex, sizeof(rec->hash_hex), hash);
    // Rows imported from /etc/shadow predate the iterated hash format and
    // carry no version marker of their own, so they are always tagged
    // legacy here. auth_login() verifies them via hash_password_legacy()
    // and lazily upgrades the in-memory record to AUTH_HASH_VERSION_ITERATED
    // on the first successful login.
    rec->hash_version = AUTH_HASH_VERSION_LEGACY;
    if (uid >= g_next_uid) {
        g_next_uid = uid + 1;
    }
    if (gid >= g_next_gid) {
        g_next_gid = gid + 1;
    }
    return AUTH_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void auth_init(void) {
    memset(g_users, 0, sizeof(g_users));
    memset(g_groups, 0, sizeof(g_groups));
    g_active_user = 0;
    g_next_uid = 1;
    g_next_gid = 0;

    int root_group = ensure_group("root");
    int users_group = ensure_group("users");
    if (root_group < 0 || users_group < 0) {
        debuglog(DEBUG_ERROR, "[AUTH] Failed to create core groups\n");
    }

    // Attempt to import /etc/shadow entries from initrd
    const uint8* data = 0;
    uint32 size = 0;
    if (vfs_read_file("/etc/shadow", &data, &size) && data && size > 0) {
        char buffer[512];
        if (size >= sizeof(buffer)) {
            size = sizeof(buffer) - 1;
        }
        memcpy(buffer, data, size);
        buffer[size] = '\0';

        char* line = strtok(buffer, "\n");
        while (line) {
            char* user = strtok(line, ":");
            char* salt = strtok(0, ":");
            char* hash = strtok(0, ":");
            char* uid = strtok(0, ":");
            char* gid = strtok(0, ":");
            char* mask = strtok(0, ":");
            if (user && salt && hash) {
                uint32 uid_v = uid ? parse_uint(uid, g_next_uid) : g_next_uid;
                uint32 gid_v = gid ? parse_uint(gid, (uint32)root_group) : (uint32)root_group;
                uint32 mask_v = mask ? parse_uint(mask, (1u << gid_v)) : (1u << gid_v);
                uint32 flags = strcmp(user, "root") == 0 ? AUTH_FLAG_ROOT : 0;
                import_user_from_shadow(user, salt, hash, uid_v, gid_v, mask_v, flags);
            }
            line = strtok(0, "\n");
        }
    }

    // Ensure root exists even if no shadow entries were imported
    if (!find_user_record("root")) {
        add_user_internal("root", DEFAULT_ROOT_PASSWORD, (uint32)root_group, AUTH_FLAG_ROOT);
    }
}

auth_result_t auth_login(const char* username, const char* password, auth_user_info_t* out_info) {
    // NOTE: intentionally no debuglog() of the raw password or of any hash
    // (stored or computed) here, even truncated -- logging password or hash
    // material, plaintext or prefix, undermines the whole point of hashing
    // credentials in the first place. Only non-sensitive outcome logging is
    // permitted below.
    user_record_t* rec = find_user_record(username);
    if (!rec) {
        return AUTH_ERR_NOT_FOUND;
    }

    char attempt_hash[AUTH_HASH_HEX_LEN];
    bool matched;
    if (rec->hash_version == AUTH_HASH_VERSION_ITERATED) {
        hash_password(rec->salt, password, attempt_hash);
        matched = hash_hex_equal(attempt_hash, rec->hash_hex);
    } else {
        // Legacy record (imported from /etc/shadow before the iterated
        // hash existed): verify with the old single-round construction so
        // pre-existing users are not locked out.
        hash_password_legacy(rec->salt, password, attempt_hash);
        matched = hash_hex_equal(attempt_hash, rec->hash_hex);
        if (matched) {
            // Lazy rehash-on-next-login migration: now that we have the
            // correct plaintext in hand, upgrade the in-memory record to
            // the iterated format so subsequent logins in this boot session
            // use the stronger scheme.
            hash_password(rec->salt, password, rec->hash_hex);
            rec->hash_version = AUTH_HASH_VERSION_ITERATED;
            debuglog(DEBUG_INFO, "[AUTH] Migrated user '%s' to iterated hash format (in-memory only; see NOTE below)\n", rec->info.name);

            // NOTE on persistence (accepted limitation, verified against the
            // current VFS): this upgrade cannot be written back to
            // /etc/shadow today, so it is lost on every reboot and the
            // record downgrades back to AUTH_HASH_VERSION_LEGACY on the next
            // boot's import_user_from_shadow() pass. This was checked, not
            // assumed: /etc/shadow is served exclusively from the boot
            // initrd (see vfs_init() in src/vfs.c, which mounts the initrd
            // as the VFS root with no writable mount layered over /etc), and
            // create_initrd_node() wires every initrd node's node->write to
            // initrd_write(), which is a hard-coded stub that unconditionally
            // returns 0 ("Write to initrd file (not supported - read-only)")
            // -- see src/vfs.c. There is consequently no vfs_write()/
            // vfs_open(..., VFS_WRITE) call auth.c could issue that would
            // actually persist bytes to /etc/shadow from this code path.
            // Making this migration durable needs one of: (a) a writable
            // backing store for /etc/shadow (e.g. mounting a real block-
            // device-backed filesystem over /etc instead of leaving it on
            // the read-only initrd), or (b) a dedicated initrd-overlay /
            // writable-shadow-copy primitive that boot-time code prefers
            // over the initrd copy when present. Until one of those exists,
            // this is intentionally left as a documented in-memory-only
            // partial fix rather than a fake write that would silently no-op.
        }
    }

    if (!matched) {
        return AUTH_ERR_BAD_CREDENTIALS;
    }

    g_active_user = rec;
    if (out_info) {
        memcpy(out_info, &rec->info, sizeof(auth_user_info_t));
    }
    return AUTH_OK;
}

auth_result_t auth_signup(const char* username, const char* password, const char* primary_group, bool elevate_to_root) {
    if (!username || !password) {
        return AUTH_ERR_INVALID;
    }
    // Reject rather than silently truncate: hash_password() only ever hashes
    // the first AUTH_PASS_MAX-1 bytes of the password, so a longer password
    // must be rejected here instead of being accepted and quietly hashed as
    // a shorter, truncated value the caller never agreed to.
    if (strlen(password) > AUTH_PASS_MAX - 1) {
        return AUTH_ERR_INVALID;
    }

    // Only root can add users once root exists
    if (find_user_record("root") && (!g_active_user || !(g_active_user->info.flags & AUTH_FLAG_ROOT))) {
        return AUTH_ERR_PERM;
    }

    int group_slot = (primary_group && primary_group[0]) ? ensure_group(primary_group) : ensure_group("users");
    if (group_slot < 0) {
        return AUTH_ERR_FULL;
    }

    uint32 gid = g_groups[group_slot].info.gid;
    uint32 flags = elevate_to_root ? AUTH_FLAG_ROOT : 0;
    return add_user_internal(username, password, gid, flags);
}

auth_result_t auth_change_password(const char* username, const char* password) {
    if (!password) {
        return AUTH_ERR_INVALID;
    }
    // See auth_signup(): reject overlong passwords instead of silently
    // truncating them to AUTH_PASS_MAX-1 bytes in hash_password().
    if (strlen(password) > AUTH_PASS_MAX - 1) {
        return AUTH_ERR_INVALID;
    }
    user_record_t* rec = find_user_record(username);
    if (!rec) {
        return AUTH_ERR_NOT_FOUND;
    }
    // Fail closed: a NULL g_active_user (no one logged in, e.g. at boot or
    // after logout) must never satisfy this guard. The previous
    // `g_active_user && ...` form short-circuited to false in that case,
    // which let any caller reach here without authentication and reset any
    // account's password -- including root's -- via USER_OP_PASSWD. Mirror
    // auth_signup()'s NULL handling for the same reason: require an active,
    // logged-in caller who is either the target account itself or root.
    if (!g_active_user || (g_active_user != rec && !(g_active_user->info.flags & AUTH_FLAG_ROOT))) {
        return AUTH_ERR_PERM;
    }

    auth_generate_salt(rec->salt);
    hash_password(rec->salt, password, rec->hash_hex);
    rec->hash_version = AUTH_HASH_VERSION_ITERATED;
    return AUTH_OK;
}

auth_result_t auth_add_group(const char* name, uint32* out_gid) {
    if (!name) {
        return AUTH_ERR_INVALID;
    }
    if (!g_active_user || !(g_active_user->info.flags & AUTH_FLAG_ROOT)) {
        return AUTH_ERR_PERM;
    }
    int slot = ensure_group(name);
    if (slot < 0) {
        return AUTH_ERR_FULL;
    }
    if (out_gid) {
        *out_gid = g_groups[slot].info.gid;
    }
    return AUTH_OK;
}

auth_result_t auth_list(auth_user_info_t* buffer, uint32 max_entries, uint32* out_count) {
    if (!buffer || max_entries == 0) {
        return AUTH_ERR_INVALID;
    }
    uint32 written = 0;
    for (uint32 i = 0; i < AUTH_MAX_USERS && written < max_entries; ++i) {
        if (g_users[i].used) {
            memcpy(&buffer[written], &g_users[i].info, sizeof(auth_user_info_t));
            written++;
        }
    }
    if (out_count) {
        *out_count = written;
    }
    return AUTH_OK;
}

auth_result_t auth_get_current(auth_user_info_t* out_info) {
    if (!g_active_user) {
        return AUTH_ERR_NOT_FOUND;
    }
    if (out_info) {
        memcpy(out_info, &g_active_user->info, sizeof(auth_user_info_t));
    }
    return AUTH_OK;
}

auth_result_t auth_logout(void) {
    g_active_user = 0;
    return AUTH_OK;
}

bool auth_user_is_admin(const auth_user_info_t* info) {
    if (!info) {
        return false;
    }
    return (info->flags & AUTH_FLAG_ROOT) || (info->groups_mask & AUTH_FLAG_ROOT);
}

auth_result_t auth_find_user(const char* username, auth_user_info_t* out_info) {
    user_record_t* rec = find_user_record(username);
    if (!rec) {
        return AUTH_ERR_NOT_FOUND;
    }
    if (out_info) {
        memcpy(out_info, &rec->info, sizeof(auth_user_info_t));
    }
    return AUTH_OK;
}

auth_result_t auth_force_login(const char* username) {
    user_record_t* rec = find_user_record(username);
    if (!rec) {
        return AUTH_ERR_NOT_FOUND;
    }
    g_active_user = rec;
    return AUTH_OK;
}

uint32 auth_active_uid(void) {
    return g_active_user ? g_active_user->info.uid : 0;
}

uint32 auth_active_gid(void) {
    return g_active_user ? g_active_user->info.gid : 0;
}

uint32 auth_active_groups_mask(void) {
    return g_active_user ? g_active_user->info.groups_mask : 0;
}

uint32 auth_get_group_gid(const char* name) {
    if (!name) return (uint32)-1;
    for (int i = 0; i < AUTH_MAX_GROUPS; ++i) {
        if (g_groups[i].used && strcmp(g_groups[i].info.name, name) == 0) {
            return g_groups[i].info.gid;
        }
    }
    return (uint32)-1;
}
