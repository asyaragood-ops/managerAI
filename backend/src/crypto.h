/* Шифрование API-ключей в базе: AES-256-GCM (OpenSSL).
 *
 * Мастер-ключ (32 байта) хранится в отдельном файле рядом с БД (по умолчанию data/master.key):
 *   - Windows: защищён DPAPI (CryptProtectData) — расшифровать его может только ваша учётная запись Windows;
 *   - Linux/macOS: файл с правами 0600.
 * Зашифрованное значение: "enc:v1:" + base64(nonce[12] | ciphertext | tag[16]). */
#ifndef IAI_CRYPTO_H
#define IAI_CRYPTO_H

#include <stddef.h>

#define CRYPTO_PREFIX "enc:v1:"

/* Загрузить мастер-ключ или создать новый. 0 — шифрование доступно. */
int  crypto_init(const char *key_path, char *err, size_t errlen);
int  crypto_available(void);
void crypto_shutdown(void);

/* Зашифровать строку. 0 — успех, результат в out. */
int  crypto_encrypt(const char *plain, char *out, size_t outlen);
/* Расшифровать значение с префиксом enc:v1:. 0 — успех. Ошибка — при подмене данных или чужом мастер-ключе. */
int  crypto_decrypt(const char *enc, char *out, size_t outlen);
int  crypto_is_encrypted(const char *s);

#endif
