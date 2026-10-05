/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Compile the production probe itself, with observable fake UEFI services.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uefi_mock.h>
#include "../../bds/MotoG20SdProbe.c"

#define MAX_VOLUMES 4u
struct fake_volume {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
  EFI_FILE_PROTOCOL root, file;
  UINT8 path[256], data[256];
  UINTN path_size, length, position, chunk;
  UINTN root_opens, file_opens, reads, bytes_returned, root_closes, file_closes;
  EFI_STATUS volume_status, open_status, fs_status, path_status;
  EFI_STATUS file_close_status, root_close_status;
  UINTN error_read_number;
  BOOLEAN null_root, null_file, oversize_read;
};
static struct fake_volume volumes[MAX_VOLUMES];
static MOTO_G20_SD_DIAGNOSTIC diagnostic;
static UINTN volume_count, connect_calls, locate_calls, pass_messages, free_calls;
static EFI_STATUS diagnostic_status, locate_status;
static unsigned cases;
static EFI_HANDLE *allocated_handles;
static const char marker[] = "MOTO G20 MICROSD READ TEST V1\r\n";
EFI_GUID gEfiSimpleFileSystemProtocolGuid = { 1, 2, 3, { 0 } };
EFI_GUID gEfiDevicePathProtocolGuid = { 4, 5, 6, { 0 } };

INTN CompareMem (CONST VOID *a, CONST VOID *b, UINTN size) { return memcmp(a, b, size); }
VOID EfiBootManagerConnectAll (VOID) { connect_calls++; }
VOID FreePool (VOID *buffer) {
  assert(buffer != NULL && buffer == allocated_handles);
  free_calls++;
  free(buffer);
  allocated_handles = NULL;
}
UINTN Print (CONST CHAR16 *format, ...) {
  char message[256];
  UINTN i = 0;
  while (format[i] != 0) {
    assert(i < sizeof(message) - 1 && format[i] <= 127);
    message[i] = (char)format[i];
    i++;
  }
  message[i] = 0;
  if (strncmp(message, "MICROSD: PASS G20SD.TXT", 21) == 0) pass_messages++;
  return i;
}

BOOLEAN IsDevicePathValid (CONST EFI_DEVICE_PATH_PROTOCOL *path, UINTN maximum) {
  UINTN i, offset = 0, size = 0;
  const UINT8 *bytes = (const UINT8 *)path;
  for (i = 0; i < volume_count; i++) {
    if (bytes == volumes[i].path) { size = volumes[i].path_size; break; }
  }
  if (size == 0 || size > maximum) return FALSE;
  while (offset + sizeof(EFI_DEVICE_PATH_PROTOCOL) <= size) {
    const EFI_DEVICE_PATH_PROTOCOL *node = (const EFI_DEVICE_PATH_PROTOCOL *)(bytes + offset);
    UINTN length = DevicePathNodeLength(node);
    if (length < sizeof(*node) || length > size - offset) return FALSE;
    if (IsDevicePathEnd(node)) return length == sizeof(*node) && offset + length == size;
    offset += length;
  }
  return FALSE;
}

static struct fake_volume *from_fs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs) {
  UINTN i;
  for (i = 0; i < volume_count; i++) if (fs == &volumes[i].fs) return &volumes[i];
  abort();
}
static struct fake_volume *from_file(EFI_FILE_PROTOCOL *file, BOOLEAN root) {
  UINTN i;
  for (i = 0; i < volume_count; i++) {
    if (file == (root ? &volumes[i].root : &volumes[i].file)) return &volumes[i];
  }
  abort();
}
static EFI_STATUS open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs, EFI_FILE_PROTOCOL **root) {
  struct fake_volume *v = from_fs(fs);
  v->root_opens++;
  if (!EFI_ERROR(v->volume_status) && !v->null_root) *root = &v->root;
  return v->volume_status;
}
static EFI_STATUS open_file(EFI_FILE_PROTOCOL *root, EFI_FILE_PROTOCOL **file,
                            CHAR16 *name, UINT64 mode, UINT64 attributes) {
  static const char expected_name[] = "\\G20SD.TXT";
  struct fake_volume *v = from_file(root, TRUE);
  UINTN i;
  v->file_opens++;
  assert(mode == EFI_FILE_MODE_READ && attributes == 0);
  for (i = 0; i < sizeof(expected_name); i++) assert(name[i] == (UINT8)expected_name[i]);
  if (!EFI_ERROR(v->open_status) && !v->null_file) *file = &v->file;
  return v->open_status;
}
static EFI_STATUS close_root(EFI_FILE_PROTOCOL *root) {
  struct fake_volume *v = from_file(root, TRUE);
  v->root_closes++;
  return v->root_close_status;
}
static EFI_STATUS close_file(EFI_FILE_PROTOCOL *file) {
  struct fake_volume *v = from_file(file, FALSE);
  v->file_closes++;
  return v->file_close_status;
}
static EFI_STATUS read_file(EFI_FILE_PROTOCOL *file, UINTN *count, VOID *data) {
  struct fake_volume *v = from_file(file, FALSE);
  UINTN transferred;
  v->reads++;
  assert(*count > 0 && *count <= 128 && *count <= 128 - v->bytes_returned);
  if (v->error_read_number == v->reads) return EFI_DEVICE_ERROR;
  if (v->oversize_read) { (*count)++; return EFI_SUCCESS; } /* Do not corrupt caller memory. */
  transferred = v->length - v->position;
  if (transferred > *count) transferred = *count;
  if (v->chunk != 0 && transferred > v->chunk) transferred = v->chunk;
  memcpy(data, v->data + v->position, transferred);
  v->position += transferred;
  v->bytes_returned += transferred;
  *count = transferred;
  return EFI_SUCCESS;
}
static EFI_STATUS locate_protocol(EFI_GUID *guid, VOID *registration, VOID **interface) {
  if (memcmp(guid, &mPlatformDiagnosticGuid, sizeof(*guid)) == 0) {
    assert(registration == NULL);
    return EFI_NOT_FOUND;
  }
  assert(registration == NULL && memcmp(guid, &mDiagnosticGuid, sizeof(*guid)) == 0);
  if (!EFI_ERROR(diagnostic_status)) *interface = &diagnostic;
  return diagnostic_status;
}
static EFI_STATUS locate_handles(EFI_LOCATE_SEARCH_TYPE type, EFI_GUID *guid, VOID *key,
                                  UINTN *count, EFI_HANDLE **handles) {
  UINTN i;
  locate_calls++;
  assert(type == ByProtocol && key == NULL);
  assert(memcmp(guid, &gEfiSimpleFileSystemProtocolGuid, sizeof(*guid)) == 0);
  if (EFI_ERROR(locate_status)) return locate_status;
  assert(volume_count > 0 && allocated_handles == NULL);
  allocated_handles = malloc(volume_count * sizeof(*allocated_handles));
  assert(allocated_handles != NULL);
  for (i = 0; i < volume_count; i++) allocated_handles[i] = &volumes[i];
  *handles = allocated_handles;
  *count = volume_count;
  return EFI_SUCCESS;
}
static EFI_STATUS handle_protocol(EFI_HANDLE handle, EFI_GUID *guid, VOID **interface) {
  struct fake_volume *v = handle;
  if (memcmp(guid, &gEfiDevicePathProtocolGuid, sizeof(*guid)) == 0) {
    if (!EFI_ERROR(v->path_status)) *interface = v->path;
    return v->path_status;
  }
  assert(memcmp(guid, &gEfiSimpleFileSystemProtocolGuid, sizeof(*guid)) == 0);
  if (!EFI_ERROR(v->fs_status)) *interface = &v->fs;
  return v->fs_status;
}
static EFI_BOOT_SERVICES services = { locate_protocol, locate_handles, handle_protocol };
EFI_BOOT_SERVICES *gBS = &services;

static void append_node(struct fake_volume *v, UINT8 type, UINT8 subtype, UINTN length) {
  EFI_DEVICE_PATH_PROTOCOL node = { type, subtype, { (UINT8)length, 0 } };
  assert(length >= sizeof(node) && v->path_size + length <= sizeof(v->path));
  memcpy(v->path + v->path_size, &node, sizeof(node));
  v->path_size += length;
}
static void make_path(struct fake_volume *v, BOOLEAN foreign, UINT8 slot, BOOLEAN partition) {
  EFI_GUID guid = mControllerGuid;
  v->path_size = 0;
  memset(v->path, 0, sizeof(v->path));
  if (foreign) guid.Data1++;
  append_node(v, HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof(VENDOR_DEVICE_PATH));
  memcpy(v->path + sizeof(EFI_DEVICE_PATH_PROTOCOL), &guid, sizeof(guid));
  append_node(v, MESSAGING_DEVICE_PATH, MSG_SD_DP, sizeof(SD_DEVICE_PATH));
  v->path[v->path_size - 1] = slot;
  if (partition) append_node(v, 4, 1, 42); /* Media/HardDrive partition node. */
  append_node(v, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(EFI_DEVICE_PATH_PROTOCOL));
}
static void reset(void) {
  UINTN i;
  assert(allocated_handles == NULL);
  memset(volumes, 0, sizeof(volumes));
  memset(&diagnostic, 0, sizeof(diagnostic));
  diagnostic.Revision = 1;
  diagnostic.Stage = 3;
  diagnostic.Published = TRUE;
  diagnostic_status = locate_status = EFI_SUCCESS;
  connect_calls = locate_calls = pass_messages = free_calls = 0;
  volume_count = 1;
  mProbed = FALSE;
  for (i = 0; i < MAX_VOLUMES; i++) {
    struct fake_volume *v = &volumes[i];
    v->fs.OpenVolume = open_volume;
    v->root.Open = open_file;
    v->root.Close = close_root;
    v->file.Close = close_file;
    v->file.Read = read_file;
    v->length = sizeof(marker) - 1;
    memcpy(v->data, marker, v->length);
    make_path(v, FALSE, 0, TRUE);
  }
}
static void finish(const char *name, BOOLEAN expected_pass) {
  assert(pass_messages == (expected_pass ? 1u : 0u));
  assert(allocated_handles == NULL);
  cases++;
  printf("HOST_OK %02u %s\n", cases, name);
}
static void no_open(void) {
  UINTN i;
  for (i = 0; i < volume_count; i++) {
    assert(volumes[i].root_opens == 0 && volumes[i].file_opens == 0 && volumes[i].reads == 0);
  }
}

int main(void) {
  struct fake_volume *v = &volumes[0];
  assert(sizeof(marker) - 1 == 31 && sizeof(mExpected) - 1 == 31);
  reset(); MotoG20SdProbe();
  assert(v->reads == 2 && v->bytes_returned == 31 && v->file_closes == 1 && v->root_closes == 1);
  assert(connect_calls == 1 && locate_calls == 1 && free_calls == 1);
  finish("exact 31 bytes plus observed EOF", TRUE);

  reset(); v->chunk = 1; MotoG20SdProbe();
  assert(v->reads == 32 && v->bytes_returned == 31); finish("partial one-byte reads followed by EOF", TRUE);
  reset(); v->length = 30; MotoG20SdProbe(); finish("truncated marker rejected", FALSE);
  reset(); v->length = 0; MotoG20SdProbe(); finish("empty file rejected", FALSE);
  reset(); v->data[12] = 'X'; MotoG20SdProbe(); finish("different content with exact length rejected", FALSE);
  reset(); v->data[29] = '\n'; v->length = 30; MotoG20SdProbe(); finish("LF-only marker rejected", FALSE);
  reset(); v->data[v->length++] = 'X'; MotoG20SdProbe(); finish("matching prefix with extra suffix rejected", FALSE);
  reset(); v->length = 256; MotoG20SdProbe();
  assert(v->bytes_returned == 128 && v->reads == 1); finish("long file capped at 128 bytes and rejected", FALSE);
  reset(); v->error_read_number = 2; MotoG20SdProbe();
  assert(v->bytes_returned == 31 && v->reads == 2); finish("EOF read error after exact marker rejected", FALSE);

  reset(); v->volume_status = EFI_DEVICE_ERROR; MotoG20SdProbe();
  assert(v->file_opens == 0 && v->root_closes == 0); finish("OpenVolume error rejected", FALSE);
  reset(); v->open_status = EFI_NOT_FOUND; MotoG20SdProbe();
  assert(v->reads == 0 && v->file_closes == 0 && v->root_closes == 1); finish("Open error closes root and rejects", FALSE);
  reset(); v->error_read_number = 1; MotoG20SdProbe();
  assert(v->file_closes == 1 && v->root_closes == 1); finish("Read error closes both handles and rejects", FALSE);
  reset(); v->file_close_status = EFI_DEVICE_ERROR; MotoG20SdProbe(); finish("file Close error rejects", FALSE);
  reset(); v->root_close_status = EFI_DEVICE_ERROR; MotoG20SdProbe(); finish("root Close error rejects", FALSE);

  reset(); make_path(v, TRUE, 0, TRUE); MotoG20SdProbe(); no_open(); finish("other vendor never opened", FALSE);
  reset(); make_path(v, FALSE, 1, TRUE); MotoG20SdProbe(); no_open(); finish("other SD slot never opened", FALSE);
  reset(); v->path[sizeof(VENDOR_DEVICE_PATH) + 1] = 0x1b; MotoG20SdProbe(); no_open();
  finish("non-SD messaging node never opened", FALSE);
  reset(); v->path[2] = 3; MotoG20SdProbe(); no_open(); finish("malformed device path rejected", FALSE);
  reset(); v->path[v->path_size - 3] = END_INSTANCE_DEVICE_PATH_SUBTYPE;
  append_node(v, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(EFI_DEVICE_PATH_PROTOCOL));
  MotoG20SdProbe(); no_open(); finish("multi-instance device path rejected", FALSE);
  reset(); make_path(v, FALSE, 0, FALSE); MotoG20SdProbe(); finish("exact SD prefix without partition also accepted", TRUE);

  reset(); diagnostic.Revision = 2; MotoG20SdProbe(); no_open();
  assert(connect_calls == 0 && locate_calls == 0); finish("wrong diagnostic revision neither connects nor opens", FALSE);
  reset(); diagnostic.Published = FALSE; MotoG20SdProbe(); no_open();
  assert(connect_calls == 0 && locate_calls == 0); finish("unpublished BlockIo neither connects nor opens", FALSE);
  reset(); diagnostic_status = EFI_NOT_FOUND; MotoG20SdProbe(); no_open();
  assert(connect_calls == 0 && locate_calls == 0); finish("missing diagnostic neither connects nor opens", FALSE);
  reset(); locate_status = EFI_NOT_FOUND; MotoG20SdProbe(); no_open();
  assert(connect_calls == 1 && free_calls == 0); finish("missing filesystem rejects without access", FALSE);
  reset(); volume_count = 2; make_path(v, TRUE, 0, TRUE); MotoG20SdProbe();
  assert(v->root_opens == 0 && volumes[1].root_opens == 1); finish("foreign identical file skipped before real SD volume", TRUE);
  reset(); v->fs_status = EFI_DEVICE_ERROR; MotoG20SdProbe(); no_open(); finish("filesystem protocol failure never opens", FALSE);
  reset(); v->path_status = EFI_NOT_FOUND; MotoG20SdProbe(); no_open(); finish("missing path protocol never opens", FALSE);

  reset(); v->null_root = TRUE; MotoG20SdProbe();
  assert(v->file_opens == 0); finish("successful OpenVolume with null root rejected", FALSE);
  reset(); v->null_file = TRUE; MotoG20SdProbe();
  assert(v->reads == 0 && v->root_closes == 1); finish("successful Open with null file rejected", FALSE);
  reset(); v->oversize_read = TRUE; MotoG20SdProbe(); finish("oversize returned count rejected", FALSE);
  reset(); MotoG20SdProbe(); MotoG20SdProbe();
  assert(connect_calls == 1 && locate_calls == 1 && v->file_opens == 1 && v->reads == 2);
  finish("second invocation does not reconnect or reopen", TRUE);
  printf("HOST PASS: %u scenarios; production probe with fake UEFI services, no physical device.\n", cases);
  return 0;
}
