/* infinityAI router — бэкенд на C.
 * Принимает сообщения чата и отправляет их по каскаду моделей: при исчерпании лимита (429, quota)
 * автоматически переключается на следующий ИИ и сообщает фронтенду, кто ответил. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "http_client.h"
#include "log.h"
#include "platform.h"
#include "quota.h"
#include "crypto.h"
#include "router.h"
#include "settings.h"
#include "server.h"
#include "strbuf.h"

static void usage(const char *argv0) {
    fprintf(stderr,
            "Использование: %s [--config config.json] [--env .env] [--port 8080] [--verbose]\n"
            "  --config  путь к конфигу каскада (по умолчанию config.json)\n"
            "  --env     файл с API-ключами KEY=VALUE (по умолчанию .env, если есть)\n"
            "  --port    переопределить порт из конфига\n"
            "  --verbose подробный лог (каждый HTTP-запрос)\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *config_path = "config.json";
    const char *env_path = ".env";
    int port = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--env") && i + 1 < argc) env_path = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) log_set_level(LOG_DEBUG);
        else { usage(argv[0]); return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 2; }
    }

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); /* русский текст в логах консоли Windows */
#endif
    config_load_env(env_path);
    app_config cfg;
    char err[512];
    if (config_load(config_path, &cfg, err, sizeof err) != 0) {
        LOGE("main", "%s", err);
        return 1;
    }
    if (port > 0) cfg.port = port;

    quota_db *q = quota_open(cfg.db_path, err, sizeof err);
    if (!q) { LOGE("main", "%s", err); config_free(&cfg); return 1; }
    if (http_global_init() != 0 || plat_net_init() != 0) {
        LOGE("main", "не удалось инициализировать сеть");
        quota_close(q);
        config_free(&cfg);
        return 1;
    }

    /* мастер-ключ шифрования лежит рядом с базой */
    char key_path[300];
    xstrlcpy(key_path, cfg.db_path, sizeof key_path);
    char *sl = strrchr(key_path, '/'), *bsl = strrchr(key_path, '\\');
    if (bsl > sl) sl = bsl;
    if (sl) strcpy(sl + 1, "master.key");
    else strcpy(key_path, "master.key");
    if (crypto_init(key_path, err, sizeof err) != 0) LOGW("main", "шифрование ключей недоступно: %s", err);

    router rt = { &cfg, q, 0 };
    LOGI("main", "Каскад: %zu моделей из config.json + %zu из каталога (подключаются автоматически, когда в «Настройках» есть ключ)",
         cfg.nexplicit, cfg.nmodels - cfg.nexplicit);
    int configured = 0, auto_ready = 0;
    for (size_t i = 0; i < cfg.nmodels; i++) {
        const model_cfg *m = &cfg.models[i];
        char kb[600];
        int has = router_api_key(&rt, m, kb, sizeof kb, NULL) != NULL;
        configured += has && m->enabled && !m->keyless;
        if (m->auto_added) { auto_ready += has; continue; }
        LOGI("main", "  %zu. %-22s %-12s %-5s %-28s %s%s", i + 1, m->label, m->provider, kind_code(model_kind(m)), m->model, m->tier,
             !m->enabled ? "  [выключена]" : m->keyless ? "  [без ключа]" : has ? "" : "  [нет ключа]");
    }
    LOGI("main", "  + моделей каталога с ключом из .env: %d (у каждого пользователя — свои ключи из «Настроек»)", auto_ready);
    if (plat_mkdirs(cfg.media_dir) != 0) LOGW("main", "не удалось создать папку для картинок/аудио/видео: %s", cfg.media_dir);
    else LOGI("main", "Картинки, аудио и видео сохраняются в %s", cfg.media_dir);
    if (!configured) LOGW("main", "Ключей в .env нет — каждый пользователь добавляет свои на сайте: Настройки → API-ключи");
    LOGI("main", "Пользователи: регистрация %s; ключи из .env доступны: %s", cfg.allow_registration ? "открыта" : "закрыта",
         !strcmp(cfg.env_keys, "all") ? "всем" : !strcmp(cfg.env_keys, "none") ? "никому" : "владельцу (первому зарегистрированному)");
    server_ctx ctx = { &cfg, &rt, q };
    int rc = server_run(&ctx);

    http_global_cleanup();
    plat_net_cleanup();
    crypto_shutdown();
    quota_close(q);
    config_free(&cfg);
    return rc == 0 ? 0 : 1;
}
