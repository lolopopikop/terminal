#ifndef VFS_H
#define VFS_H

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

int start_users_vfs(const char *mount_point);
void stop_users_vfs();

#endif