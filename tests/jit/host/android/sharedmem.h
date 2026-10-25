// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <sys/mman.h>
#include <unistd.h>
// Linux memfd substitutes only the Android fd factory, retaining real shared pages.
inline int ASharedMemory_create(const char *name,size_t size) {
    const int fd=memfd_create(name,0);
    if(fd>=0 && ftruncate(fd,size)) { close(fd); return -1; }
    return fd;
}
