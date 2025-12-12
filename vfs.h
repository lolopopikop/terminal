#ifndef VFS_H
#define VFS_H

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

int start_users_vfs(const char *mount_point);
void stop_users_vfs();
int vfs_add_user(const char *username);
int vfs_user_exists(const char *username);

#endif