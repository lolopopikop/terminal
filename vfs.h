#ifndef VFS_H
#define VFS_H

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

int start_users_vfs(const char *mount_point);
void stop_users_vfs();

/* FUSE operations */
int vfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int vfs_mkdir(const char *path, mode_t mode);
int vfs_rmdir(const char *path);
int vfs_open(const char *path, struct fuse_file_info *fi);
int vfs_read(const char *path, char *buf, size_t size, off_t offset,
             struct fuse_file_info *fi);
int vfs_write(const char *path, const char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi);

#endif