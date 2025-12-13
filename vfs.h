#ifndef VFS_H
#define VFS_H

#ifdef __cplusplus
extern "C" {
#endif

int start_users_vfs(const char *mount_point);
void stop_users_vfs(void);

#ifdef __cplusplus
}
#endif

#endif
