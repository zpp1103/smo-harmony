#pragma once
/* posix_spawn shim: OHOS musl 无 posix_spawn，watchdog 降级为 no-op */
#include <sys/types.h>
#define POSIX_SPAWN_CLOEXEC_DEFAULT 0x1000
#define POSIX_SPAWN_SETSIGMASK 0x0400
typedef struct { int __unused[16]; } posix_spawn_file_actions_t;
typedef struct { int __unused[16]; } posix_spawnattr_t;
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);
