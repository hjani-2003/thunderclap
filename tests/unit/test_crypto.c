#include "crypt.h"
#include "proto.h"
#include "check.h"

#include <sys/socket.h>

static void test_cpace(void)
{
    uint8_t sid[SID_LEN], k1[KEY_LEN], k2[KEY_LEN];
    randombytes_buf(sid, sizeof sid);

    cpace_t a, b;
    CHECK(cpace_start(&a, "1234-5678-9012", sid) == 0);
    CHECK(cpace_start(&b, "1234 5678 9012", sid) == 0);   /* separators don't matter */
    uint8_t ya[32], yb[32];
    memcpy(ya, a.Y, 32);
    memcpy(yb, b.Y, 32);
    CHECK(cpace_finish(&a, yb, true, k1) == 0);
    CHECK(cpace_finish(&b, ya, false, k2) == 0);
    CHECK(memcmp(k1, k2, KEY_LEN) == 0);

    /* wrong code ⇒ different keys */
    CHECK(cpace_start(&a, "1234-5678-9012", sid) == 0);
    CHECK(cpace_start(&b, "1234-5678-9013", sid) == 0);
    memcpy(ya, a.Y, 32);
    memcpy(yb, b.Y, 32);
    CHECK(cpace_finish(&a, yb, true, k1) == 0);
    CHECK(cpace_finish(&b, ya, false, k2) == 0);
    CHECK(memcmp(k1, k2, KEY_LEN) != 0);

    /* too-short codes and invalid points are refused */
    CHECK(cpace_start(&a, "12", sid) < 0);
    uint8_t zero[32] = { 0 };
    CHECK(cpace_start(&a, "123456", sid) == 0);
    CHECK(cpace_finish(&a, zero, true, k1) < 0);

    char code[15];
    pair_code_generate(code);
    CHECK(strlen(code) == 14 && code[4] == '-' && code[9] == '-');
}

static void test_kk(void)
{
    identity_t i, r, mallory;
    crypto_box_keypair(i.pk, i.sk);
    crypto_box_keypair(r.pk, r.sk);
    crypto_box_keypair(mallory.pk, mallory.sk);
    uint8_t ie_pk[32], ie_sk[32], re_pk[32], re_sk[32], m1[32], m2[32];
    crypto_box_keypair(ie_pk, ie_sk);
    crypto_box_keypair(re_pk, re_sk);

    CHECK(kk_initiator(&i, r.pk, ie_sk, ie_pk, re_pk, m1) == 0);
    CHECK(kk_responder(&r, i.pk, re_sk, re_pk, ie_pk, m2) == 0);
    CHECK(memcmp(m1, m2, 32) == 0);

    /* an impostor without r's secret key derives something else */
    CHECK(kk_responder(&mallory, i.pk, re_sk, re_pk, ie_pk, m2) == 0);
    CHECK(memcmp(m1, m2, 32) != 0);
}

static void test_channel(int cipher)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    uint8_t master[32], salt[SID_LEN];
    randombytes_buf(master, sizeof master);
    randombytes_buf(salt, sizeof salt);
    chan_t a, b;
    chan_init(&a, sv[0], cipher, master, "data", 3, salt, true);
    chan_init(&b, sv[1], cipher, master, "data", 3, salt, false);

    /* round trip, both directions */
    const char *msg = "hello thunderclap";
    CHECK(chan_send_copy(&a, msg, strlen(msg)) == 0);
    size_t n;
    uint8_t *got = chan_recv(&b, &n);
    CHECK(got && n == strlen(msg) && !memcmp(got, msg, n));
    CHECK(chan_send_copy(&b, "pong", 4) == 0);
    got = chan_recv(&a, &n);
    CHECK(got && n == 4 && !memcmp(got, "pong", 4));

    /* a large record */
    size_t big = 1 << 20;
    uint8_t *buf = malloc(big), *copy = malloc(big);
    randombytes_buf(copy, big);
    memcpy(buf, copy, big);
    /* socketpair buffers are small; send from a thread-free loop by
     * alternating isn't possible, so enlarge the buffer instead */
    int sz = 4 << 20;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    CHECK(chan_send(&a, buf, big) == 0);
    got = chan_recv(&b, &n);
    CHECK(got && n == big && !memcmp(got, copy, big));

    /* tampering is detected */
    CHECK(chan_send_copy(&a, msg, strlen(msg)) == 0);
    uint8_t raw[64];
    ssize_t r = read(sv[1], raw, sizeof raw);
    CHECK(r == 4 + (ssize_t)strlen(msg) + TAG_LEN);
    raw[6] ^= 1;
    CHECK(write(sv[0], raw, (size_t)r) == r);   /* loop it back into the same direction */
    CHECK(chan_recv(&b, &n) == NULL);

    free(buf);
    free(copy);
    chan_free(&a);
    chan_free(&b);
    close(sv[0]);
    close(sv[1]);
}

static void test_replay(void)
{
    /* A record replayed into a fresh stream with a different salt fails. */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    uint8_t master[32], s1[SID_LEN], s2[SID_LEN];
    randombytes_buf(master, 32);
    randombytes_buf(s1, SID_LEN);
    randombytes_buf(s2, SID_LEN);
    chan_t a, b;
    chan_init(&a, sv[0], CIPHER_CHACHA, master, "data", 0, s1, true);
    chan_init(&b, sv[1], CIPHER_CHACHA, master, "data", 0, s2, false);
    CHECK(chan_send_copy(&a, "x", 1) == 0);
    size_t n;
    CHECK(chan_recv(&b, &n) == NULL);
    chan_free(&a);
    chan_free(&b);
    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    CHECK(crypt_init() == 0);
    g_log_level = LOG_ERR;   /* the tamper tests log expected warnings */
    test_cpace();
    test_kk();
    test_channel(CIPHER_CHACHA);
    if (crypt_aes_available())
        test_channel(CIPHER_AES);
    test_replay();
    DONE();
}
