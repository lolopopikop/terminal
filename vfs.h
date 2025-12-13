#ifndef VFS_H
#define VFS_H

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#ifdef __cplusplus
extern "C" {
#endif

int start_users_vfs(const char *mount_point);
void stop_users_vfs(void);

int vfs_add_user(const char *username);
int vfs_user_exists(const char *username);
void vfs_list_users(void (*callback)(const char *));

#ifdef __cplusplus
}
#endif

#endif
