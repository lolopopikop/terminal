# Kubsh Shell

Custom shell with VFS (Virtual File System) support for user information.

## Features

1. Basic shell functionality
2. Command history with saving to `~/.kubsh_history`
3. Environment variable support
4. VFS mounted at `users/` directory showing user information
5. Automatic user creation/deletion via VFS operations

## Build Instructions

### Dependencies

```bash
# Ubuntu/Debian
sudo apt-get install g++ make fakeroot libfuse3-dev libreadline-dev

# Arch Linux
sudo pacman -S gcc make fakeroot fuse3 readline