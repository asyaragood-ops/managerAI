/* Платформенный слой: сокеты, время, переменные окружения, каталоги — Windows (MinGW) и POSIX. */
#ifndef IAI_PLATFORM_H
#define IAI_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
#  define SEND_FLAGS 0
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
#  ifdef MSG_NOSIGNAL
#    define SEND_FLAGS MSG_NOSIGNAL
#  else
#    define SEND_FLAGS 0
#  endif
#endif

/* Инициализация сетевой подсистемы (WSAStartup на Windows, игнор SIGPIPE на POSIX). */
int  plat_net_init(void);
void plat_net_cleanup(void);

/* Таймаут чтения/записи сокета в миллисекундах. */
void plat_sock_timeouts(sock_t s, int ms);

/* Текущее время: секунды и миллисекунды с эпохи. */
int64_t plat_now(void);
int64_t plat_now_ms(void);

/* Потокобезопасный gmtime. */
void plat_gmtime(time_t t, struct tm *out);

/* setenv, не перезаписывающий уже заданную переменную. */
void plat_setenv_if_unset(const char *name, const char *value);

/* Создать каталог (и родительские), если его нет. 0 — успех. */
int plat_mkdirs(const char *path);

/* Пауза в миллисекундах. */
void plat_sleep_ms(int ms);

#endif
