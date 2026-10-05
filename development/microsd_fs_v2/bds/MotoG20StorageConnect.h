/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_G20_STORAGE_CONNECT_H
#define MOTO_G20_STORAGE_CONNECT_H
#include <Uefi.h>

/* One bounded diagnostic/connection pass. No image loading, file I/O or writes.
 * SUCCESS means an SD partition exposes SimpleFileSystem, not a proven read. */
EFI_STATUS MotoG20StorageConnect (VOID);

#endif
