/* Copyright (c) 2014, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <openssl/hkdf.h>

#include <assert.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/hmac.h>

#include "../../internal.h"
#include "../service_indicator/internal.h"

// TODO(CryptoAlg-1281): We need to get our FIPS testing partner's opinion on
// which API level(s) we need to check at. HKDF_extract() originally had checks
// similar to HKDF_expand(), but we were unsure whether it was required at that
// level.

int HKDF(uint8_t *out_key, size_t out_len, const EVP_MD *digest,
         const uint8_t *secret, size_t secret_len, const uint8_t *salt,
         size_t salt_len, const uint8_t *info, size_t info_len) {
  // https://tools.ietf.org/html/rfc5869#section-2
  uint8_t prk[EVP_MAX_MD_SIZE];
  size_t prk_len;
  int ret = 1;

  // We have to avoid the underlying HKDF services updating the indicator
  // state, so we lock the state here.
  FIPS_service_indicator_lock_state();

  if (!HKDF_extract(prk, &prk_len, digest, secret, secret_len, salt,
                    salt_len) ||
      !HKDF_expand(out_key, out_len, digest, prk, prk_len, info, info_len)) {
    ret = 0;
    goto out;
  }

out:
  FIPS_service_indicator_unlock_state();
  HKDF_verify_service_indicator(digest, salt, salt_len, info_len);

  return ret;
}

int HKDF_extract(uint8_t *out_key, size_t *out_len, const EVP_MD *digest,
                 const uint8_t *secret, size_t secret_len, const uint8_t *salt,
                 size_t salt_len) {
  // https://tools.ietf.org/html/rfc5869#section-2.2
  int ret = 0;

  // We have to avoid the underlying HMAC services updating the indicator
  // state, so we lock the state here.
  FIPS_service_indicator_lock_state();

  // If salt is not given, HashLength zeros are used. However, HMAC does that
  // internally already so we can ignore it.
  unsigned len;
  if (HMAC(digest, salt, salt_len, secret, secret_len, out_key, &len) == NULL) {
    OPENSSL_PUT_ERROR(HKDF, ERR_R_HMAC_LIB);
    goto out;
  }
  *out_len = len;
  assert(*out_len == EVP_MD_size(digest));
  ret = 1;

out:
  FIPS_service_indicator_unlock_state();
  return ret;
}

int HKDF_expand(uint8_t *out_key, size_t out_len, const EVP_MD *digest,
                const uint8_t *prk, size_t prk_len, const uint8_t *info,
                size_t info_len) {
  // https://tools.ietf.org/html/rfc5869#section-2.3
  const size_t digest_len = EVP_MD_size(digest);
  uint8_t previous[EVP_MAX_MD_SIZE];
  size_t n, done = 0;
  unsigned i;
  int ret = 0;
  HMAC_CTX hmac;

  // Expand key material to desired length.
  n = (out_len + digest_len - 1) / digest_len;
  if (out_len + digest_len < out_len || n > 255) {
    OPENSSL_PUT_ERROR(HKDF, HKDF_R_OUTPUT_TOO_LARGE);
    return 0;
  }

  HMAC_CTX_init(&hmac);

  // We have to avoid the underlying HMAC services updating the indicator
  // state, so we lock the state here.
  FIPS_service_indicator_lock_state();

  if (!HMAC_Init_ex(&hmac, prk, prk_len, digest, NULL)) {
    goto out;
  }

  for (i = 0; i < n; i++) {
    uint8_t ctr = i + 1;
    size_t todo;

    if (i != 0 && (!HMAC_Init_ex(&hmac, NULL, 0, NULL, NULL) ||
                   !HMAC_Update(&hmac, previous, digest_len))) {
      goto out;
    }
    if (!HMAC_Update(&hmac, info, info_len) ||
        !HMAC_Update(&hmac, &ctr, 1) ||
        !HMAC_Final(&hmac, previous, NULL)) {
      goto out;
    }

    todo = digest_len;
    if (todo > out_len - done) {
      todo = out_len - done;
    }
    OPENSSL_memcpy(out_key + done, previous, todo);
    done += todo;
  }

  ret = 1;

out:
  FIPS_service_indicator_unlock_state();
  HKDFExpand_verify_service_indicator(digest);

  HMAC_CTX_cleanup(&hmac);
  if (ret != 1) {
    OPENSSL_PUT_ERROR(HKDF, ERR_R_HMAC_LIB);
  }
  return ret;
}
