/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_G20_STORAGE_CONNECT_H
#define MOTO_G20_STORAGE_CONNECT_H
#include <Uefi.h>

/* One bounded diagnostic/connection pass. No image loading, file I/O or writes.
 * Prioritizes three existing driver images on the unique native SD root, then
 * its partition handles. ConnectController may try other resident bindings;
 * the priority list is not an exclusive allowlist. It is never recursive.
 * SUCCESS means a present SD partition exposes SimpleFileSystem, not a proven
 * read or a Windows boot. Missing/ambiguous bindings prevent connection. */
EFI_STATUS MotoG20StorageConnect (VOID);

#endif
