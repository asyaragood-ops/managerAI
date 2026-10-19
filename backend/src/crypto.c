#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "log.h"
#include "platform.h"
#include "strbuf.h"

#ifdef _WIN32
#  include <wincrypt.h>
#else
#  include <sys/stat.h>
#endif

#define KEY_LEN 32
#define IV_LEN  12
#define TAG_LEN 16

static unsigned char g_key[KEY_LEN];
static int g_ready = 0;

int crypto_available(void) { return g_ready; }
int crypto_is_encrypted(const char *s) { return s && !strncmp(s, CRYPTO_PREFIX, sizeof CRYPTO_PREFIX - 1); }

/* ---------- чтение/запись файла мастер-ключа ---------- */

static int read_all(const char *path, unsigned char *buf, size_t cap, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    *len = fread(buf, 1, cap, f);
    int ok = !ferror(f);
    fclose(f);
    return ok ? 0 : -1;
}

static int write_all(const char *path, const unsigned char *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    int ok = (w == len) && fclose(f) == 0;
#ifndef _WIN32
    if (ok) chmod(path, 0600);
#endif
    return ok ? 0 : -1;
}

#ifdef _WIN32
/* DPAPI: защищаем мастер-ключ учётной записью Windows */
static int protect(const unsigned char *in, size_t inlen, unsigned char *out, size_t cap, size_t *outlen) {
    DATA_BLOB src = { (DWORD)inlen, (BYTE *)in }, dst = { 0, NULL };
    if (!CryptProtectData(&src, L"infinityAI master key", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &dst)) return -1;
    int rc = -1;
    if (dst.cbData <= cap) { memcpy(out, dst.pbData, dst.cbData); *outlen = dst.cbData; rc = 0; }
    LocalFree(dst.pbData);
    return rc;
}
static int unprotect(const unsigned char *in, size_t inlen, unsigned char *out, size_t cap, size_t *outlen) {
    DATA_BLOB src = { (DWORD)inlen, (BYTE *)in }, dst = { 0, NULL };
    if (!CryptUnprotectData(&src, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &dst)) return -1;
    int rc = -1;
    if (dst.cbData <= cap) { memcpy(out, dst.pbData, dst.cbData); *outlen = dst.cbData; rc = 0; }
    SecureZeroMemory(dst.pbData, dst.cbData);
    LocalFree(dst.pbData);
    return rc;
}
#endif

int crypto_init(const char *key_path, char *err, size_t errlen) {
    unsigned char file[1024];
    size_t flen = 0;
    if (read_all(key_path, file, sizeof file, &flen) == 0 && flen > 0) {
#ifdef _WIN32
        size_t klen = 0;
        if (unprotect(file, flen, g_key, sizeof g_key, &klen) != 0 || klen != KEY_LEN) {
            snprintf(err, errlen, "%s: не удалось расшифровать мастер-ключ (файл от другой учётной записи Windows?)", key_path);
            return -1;
        }
#else
        if (flen != KEY_LEN) { snprintf(err, errlen, "%s: повреждённый мастер-ключ", key_path); return -1; }
        memcpy(g_key, file, KEY_LEN);
#endif
        g_ready = 1;
        return 0;
    }
    /* создаём новый ключ */
    if (RAND_bytes(g_key, KEY_LEN) != 1) { snprintf(err, errlen, "нет источника случайных чисел"); return -1; }
#ifdef _WIN32
    size_t plen = 0;
    if (protect(g_key, KEY_LEN, file, sizeof file, &plen) != 0) { snprintf(err, errlen, "DPAPI: не удалось защитить ключ"); return -1; }
    if (write_all(key_path, file, plen) != 0) { snprintf(err, errlen, "не удалось записать %s", key_path); return -1; }
#else
    if (write_all(key_path, g_key, KEY_LEN) != 0) { snprintf(err, errlen, "не удалось записать %s", key_path); return -1; }
#endif
    LOGI("crypto", "создан мастер-ключ шифрования: %s", key_path);
    g_ready = 1;
    return 0;
}

void crypto_shutdown(void) {
    OPENSSL_cleanse(g_key, sizeof g_key);
    g_ready = 0;
}

int crypto_encrypt(const char *plain, char *out, size_t outlen) {
    if (!g_ready) return -1;
    size_t n = strlen(plain);
    if (n > 2048) return -1;
    unsigned char buf[IV_LEN + 2048 + TAG_LEN];
    unsigned char *iv = buf, *ct = buf + IV_LEN;
    if (RAND_bytes(iv, IV_LEN) != 1) return -1;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0, ok = 0;
    if (ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL, g_key, iv) == 1 &&
        EVP_EncryptUpdate(ctx, NULL, &len, (const unsigned char *)CRYPTO_PREFIX, (int)strlen(CRYPTO_PREFIX)) == 1 && /* AAD */
        EVP_EncryptUpdate(ctx, ct, &len, (const unsigned char *)plain, (int)n) == 1) {
        total = len;
        if (EVP_EncryptFinal_ex(ctx, ct + total, &len) == 1) {
            total += len;
            ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, ct + total) == 1;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return -1;
    size_t raw = IV_LEN + (size_t)total + TAG_LEN;
    size_t need = strlen(CRYPTO_PREFIX) + 4 * ((raw + 2) / 3) + 1;
    if (need > outlen) return -1;
    strcpy(out, CRYPTO_PREFIX);
    EVP_EncodeBlock((unsigned char *)out + strlen(CRYPTO_PREFIX), buf, (int)raw);
    OPENSSL_cleanse(buf, sizeof buf);
    return 0;
}

int crypto_decrypt(const char *enc, char *out, size_t outlen) {
    if (!g_ready || !crypto_is_encrypted(enc)) return -1;
    const char *b64 = enc + strlen(CRYPTO_PREFIX);
    size_t bl = strlen(b64);
    if (bl == 0 || bl % 4 || bl > 4096) return -1;
    unsigned char buf[3200];
    int raw = EVP_DecodeBlock(buf, (const unsigned char *)b64, (int)bl);
    if (raw < 0) return -1;
    raw -= (b64[bl - 1] == '=') + (bl > 1 && b64[bl - 2] == '='); /* EVP_DecodeBlock не учитывает паддинг */
    if (raw < IV_LEN + TAG_LEN) return -1;
    int ctlen = raw - IV_LEN - TAG_LEN;
    if ((size_t)ctlen + 1 > outlen) return -1;
    unsigned char *iv = buf, *ct = buf + IV_LEN, *tag = buf + IV_LEN + ctlen;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0, ok = 0;
    if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) == 1 &&
        EVP_DecryptInit_ex(ctx, NULL, NULL, g_key, iv) == 1 &&
        EVP_DecryptUpdate(ctx, NULL, &len, (const unsigned char *)CRYPTO_PREFIX, (int)strlen(CRYPTO_PREFIX)) == 1 &&
        EVP_DecryptUpdate(ctx, (unsigned char *)out, &len, ct, ctlen) == 1) {
        total = len;
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, tag);
        if (EVP_DecryptFinal_ex(ctx, (unsigned char *)out + total, &len) == 1) { total += len; ok = 1; } /* проверка тега */
    }
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(buf, sizeof buf);
    if (!ok) { OPENSSL_cleanse(out, outlen); out[0] = '\0'; return -1; }
    out[total] = '\0';
    return 0;
}
