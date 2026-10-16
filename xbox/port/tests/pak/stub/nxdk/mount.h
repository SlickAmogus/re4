// host stand-in for nxdk/mount.h (pak_test)
#pragma once
static inline bool nxIsDriveMounted(char) { return true; }
static inline bool nxMountDrive(char, const char*) { return true; }
