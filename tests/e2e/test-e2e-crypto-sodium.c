/*
 * A randomised differential of the native E2E crypto (src/e2e/e2e-crypto.c,
 * built on OpenSSL) against libsodium - the reference the RPE2E protocol is
 * defined over and that repartee and rpe2e.pl use. test-e2e-crypto.c pins
 * each primitive to fixed vectors; this sweeps many random inputs so an
 * input-dependent divergence between the two backends cannot hide. Built
 * only when libsodium is present (meson skips it otherwise).
 */
#include <irssi/src/common.h>
#include <irssi/src/e2e/e2e-crypto.h>
#include <sodium.h>

#define ITERS 3000

static GRand *rng;

static void fill(unsigned char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char) g_rand_int_range(rng, 0, 256);
}

/* HKDF-SHA256 (RFC 5869) from libsodium's HMAC, the reference for
 * e2e_hkdf_sha256 */
static void sodium_hkdf(const unsigned char *salt, size_t saltlen,
                        const unsigned char *ikm, size_t ikmlen,
                        const unsigned char *info, size_t infolen,
                        unsigned char *out, size_t outlen)
{
	crypto_auth_hmacsha256_state st;
	unsigned char prk[32], t[32];
	size_t done = 0, tlen = 0;
	unsigned char counter = 0;

	crypto_auth_hmacsha256_init(&st, salt, saltlen);
	crypto_auth_hmacsha256_update(&st, ikm, ikmlen);
	crypto_auth_hmacsha256_final(&st, prk);

	while (done < outlen) {
		size_t take;

		counter++;
		crypto_auth_hmacsha256_init(&st, prk, sizeof(prk));
		crypto_auth_hmacsha256_update(&st, t, tlen);
		crypto_auth_hmacsha256_update(&st, info, infolen);
		crypto_auth_hmacsha256_update(&st, &counter, 1);
		crypto_auth_hmacsha256_final(&st, t);
		tlen = sizeof(t);
		take = MIN(outlen - done, sizeof(t));
		memcpy(out + done, t, take);
		done += take;
	}
}

static void test_hashes(void)
{
	int i;

	for (i = 0; i < ITERS; i++) {
		unsigned char buf[300], a[64], b[64];
		size_t n = g_rand_int_range(rng, 0, sizeof(buf) + 1);

		fill(buf, n);
		g_assert_true(e2e_sha256(buf, n, a));
		crypto_hash_sha256(b, buf, n);
		g_assert_cmpint(memcmp(a, b, 32), ==, 0);
		g_assert_true(e2e_sha512(buf, n, a));
		crypto_hash_sha512(b, buf, n);
		g_assert_cmpint(memcmp(a, b, 64), ==, 0);
	}
}

static void test_hkdf(void)
{
	int i;

	for (i = 0; i < ITERS; i++) {
		unsigned char salt[40], ikm[80], info[40], a[160], b[160];
		size_t saltlen = g_rand_int_range(rng, 1, sizeof(salt) + 1);
		size_t ikmlen = g_rand_int_range(rng, 1, sizeof(ikm) + 1);
		size_t infolen = g_rand_int_range(rng, 0, sizeof(info) + 1);
		size_t outlen = g_rand_int_range(rng, 1, sizeof(a) + 1);

		fill(salt, saltlen);
		fill(ikm, ikmlen);
		fill(info, infolen);
		g_assert_true(e2e_hkdf_sha256(salt, saltlen, ikm, ikmlen, info, infolen, a, outlen));
		sodium_hkdf(salt, saltlen, ikm, ikmlen, info, infolen, b, outlen);
		g_assert_cmpint(memcmp(a, b, outlen), ==, 0);
	}
}

static void test_ed25519(void)
{
	int i;

	for (i = 0; i < ITERS; i++) {
		unsigned char seed[32], pk[32], sk[64], spk[32], ssk[64];
		unsigned char msg[200], sig[64], xpk[32], xsk[32], sxpk[32], sxsk[32];
		size_t mlen = g_rand_int_range(rng, 0, sizeof(msg) + 1);

		fill(seed, sizeof(seed));
		fill(msg, mlen);
		g_assert_true(e2e_ed25519_seed_keypair(seed, pk, sk));
		crypto_sign_seed_keypair(spk, ssk, seed);
		g_assert_cmpint(memcmp(pk, spk, 32), ==, 0);
		g_assert_cmpint(memcmp(sk, ssk, 64), ==, 0);

		g_assert_true(e2e_ed25519_sign(sig, msg, mlen, sk));
		/* libsodium accepts our signature and we accept libsodium's */
		g_assert_cmpint(crypto_sign_verify_detached(sig, msg, mlen, pk), ==, 0);
		crypto_sign_detached(sig, NULL, msg, mlen, sk);
		g_assert_true(e2e_ed25519_verify(sig, msg, mlen, pk));

		g_assert_true(e2e_ed25519_pk_to_x25519(xpk, pk));
		g_assert_cmpint(crypto_sign_ed25519_pk_to_curve25519(sxpk, pk), ==, 0);
		g_assert_cmpint(memcmp(xpk, sxpk, 32), ==, 0);
		g_assert_true(e2e_ed25519_sk_to_x25519(xsk, sk));
		g_assert_cmpint(crypto_sign_ed25519_sk_to_curve25519(sxsk, sk), ==, 0);
		g_assert_cmpint(memcmp(xsk, sxsk, 32), ==, 0);
	}
}

static void test_x25519(void)
{
	int i;

	for (i = 0; i < ITERS; i++) {
		unsigned char a_sk[32], a_pk[32], b_sk[32], b_pk[32];
		unsigned char ours[32], theirs[32], pub[32];

		fill(a_sk, sizeof(a_sk));
		fill(b_sk, sizeof(b_sk));
		g_assert_true(e2e_x25519_public(a_pk, a_sk));
		crypto_scalarmult_base(pub, a_sk);
		g_assert_cmpint(memcmp(a_pk, pub, 32), ==, 0);
		crypto_scalarmult_base(b_pk, b_sk);

		/* the shared secret both ways, each backend against the other */
		g_assert_true(e2e_x25519(ours, a_sk, b_pk));
		g_assert_cmpint(crypto_scalarmult(theirs, b_sk, a_pk), ==, 0);
		g_assert_cmpint(memcmp(ours, theirs, 32), ==, 0);
	}
}

static void test_xchacha(void)
{
	int i;

	for (i = 0; i < ITERS; i++) {
		unsigned char key[32], nonce[24], aad[48], pt[200];
		unsigned char ours[200 + 16], theirs[200 + 16], back[200];
		unsigned long long clen = 0;
		size_t ptlen = g_rand_int_range(rng, 0, sizeof(pt) + 1);
		size_t aadlen = g_rand_int_range(rng, 0, sizeof(aad) + 1);
		unsigned char hc_in[16], hc_ours[32], hc_theirs[32];

		fill(key, sizeof(key));
		fill(nonce, sizeof(nonce));
		fill(aad, aadlen);
		fill(pt, ptlen);

		/* HChaCha20 core */
		fill(hc_in, sizeof(hc_in));
		e2e_hchacha20(hc_ours, key, hc_in);
		crypto_core_hchacha20(hc_theirs, hc_in, key, NULL);
		g_assert_cmpint(memcmp(hc_ours, hc_theirs, 32), ==, 0);

		/* XChaCha20-Poly1305 IETF: ciphertext||tag, both layouts equal */
		g_assert_true(e2e_xchacha_encrypt(ours, pt, ptlen, aad, aadlen, nonce, key));
		crypto_aead_xchacha20poly1305_ietf_encrypt(theirs, &clen, pt, ptlen, aad, aadlen,
		                                           NULL, nonce, key);
		g_assert_cmpint((size_t) clen, ==, ptlen + 16);
		g_assert_cmpint(memcmp(ours, theirs, ptlen + 16), ==, 0);
		/* our decrypt reads libsodium's ciphertext */
		g_assert_true(e2e_xchacha_decrypt(back, theirs, ptlen + 16, aad, aadlen, nonce, key));
		if (ptlen > 0)
			g_assert_cmpint(memcmp(back, pt, ptlen), ==, 0);
	}
}

int main(int argc, char **argv)
{
	int r;

	g_assert_cmpint(sodium_init(), >=, 0);
	g_test_init(&argc, &argv, NULL);
	rng = g_rand_new_with_seed(0x5E11E7E2);
	g_test_add_func("/e2e/crypto-sodium/hashes", test_hashes);
	g_test_add_func("/e2e/crypto-sodium/hkdf", test_hkdf);
	g_test_add_func("/e2e/crypto-sodium/ed25519", test_ed25519);
	g_test_add_func("/e2e/crypto-sodium/x25519", test_x25519);
	g_test_add_func("/e2e/crypto-sodium/xchacha", test_xchacha);
	r = g_test_run();
	g_rand_free(rng);
	return r;
}
