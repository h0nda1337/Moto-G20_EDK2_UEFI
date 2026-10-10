/* Compile the production raw probe with observable BlockIo/path services. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uefi_mock.h>
#include "../../bds/MotoG20RawSdProbe.c"

#define MAX_DISKS 3u
struct fake_disk {
  EFI_BLOCK_IO_PROTOCOL block;
  EFI_BLOCK_IO_MEDIA media, replacement;
  UINT8 path[128];
  UINTN path_size, reads;
  EFI_STATUS path_status, block_status;
  BOOLEAN null_path, null_block;
};
static struct fake_disk disks[MAX_DISKS];
static EFI_HANDLE *allocated_handles;
static UINTN disk_count, total_reads, pass_sectors, pass_all, frees, cases;
static EFI_STATUS locate_status, read_status;
static BOOLEAN null_handles, zero_count;
static UINTN problem_read, missing_byte, changed_byte;
static unsigned behavior, mutation;
enum { EXACT, ERROR, NO_COPY, PARTIAL, MISMATCH, MEDIA_CHANGE, ZERO_FILL };
static const UINT64 expected_lbas[] = { 0, 2048, 99072 };
EFI_GUID gEfiBlockIoProtocolGuid = {1, 2, 3, {0}};
EFI_GUID gEfiDevicePathProtocolGuid = {4, 5, 6, {0}};

INTN CompareMem (CONST VOID *a, CONST VOID *b, UINTN n) { return memcmp(a, b, n); }
VOID FreePool (VOID *buffer) {
  assert(buffer != NULL && buffer == allocated_handles);
  frees++;
  free(buffer);
  allocated_handles = NULL;
}
UINTN Print (CONST CHAR16 *format, ...) {
  char text[256];
  UINTN i = 0;
  while (format[i] != 0) {
    assert(i < sizeof(text) - 1 && format[i] <= 127);
    text[i] = (char)format[i];
    i++;
  }
  text[i] = 0;
  if (strstr(text, "SDRAW: PASS LBA=") != NULL) pass_sectors++;
  if (strstr(text, "SDRAW: PASS all 3 sectors") != NULL) pass_all++;
  return i;
}
BOOLEAN IsDevicePathValid (CONST EFI_DEVICE_PATH_PROTOCOL *path, UINTN maximum) {
  const UINT8 *bytes = (const UINT8 *)path;
  UINTN i, size = 0, offset = 0;
  for (i = 0; i < disk_count; i++) {
    if (bytes == disks[i].path) { size = disks[i].path_size; break; }
  }
  if (size == 0 || size > maximum) return FALSE;
  while (offset + sizeof(*path) <= size) {
    const EFI_DEVICE_PATH_PROTOCOL *node = (const EFI_DEVICE_PATH_PROTOCOL *)(bytes + offset);
    UINTN length = DevicePathNodeLength(node);
    if (length < sizeof(*node) || length > size - offset) return FALSE;
    if (IsDevicePathEnd(node)) return length == sizeof(*node) && offset + length == size;
    offset += length;
  }
  return FALSE;
}
static void append_node(struct fake_disk *d, UINT8 type, UINT8 subtype, UINTN length) {
  EFI_DEVICE_PATH_PROTOCOL node = {type, subtype, {(UINT8)length, 0}};
  assert(length >= sizeof(node) && length < 256 && d->path_size + length <= sizeof(d->path));
  memcpy(d->path + d->path_size, &node, sizeof(node));
  d->path_size += length;
}
static void make_path(struct fake_disk *d, BOOLEAN foreign, UINT8 slot, BOOLEAN partition) {
  EFI_GUID guid = mControllerGuid;
  if (foreign) guid.Data1++;
  d->path_size = 0;
  memset(d->path, 0, sizeof(d->path));
  append_node(d, HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof(VENDOR_DEVICE_PATH));
  memcpy(d->path + sizeof(EFI_DEVICE_PATH_PROTOCOL), &guid, sizeof(guid));
  append_node(d, MESSAGING_DEVICE_PATH, MSG_SD_DP, sizeof(SD_DEVICE_PATH));
  d->path[d->path_size - 1] = slot;
  if (partition) append_node(d, 4, 1, 42);
  append_node(d, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(EFI_DEVICE_PATH_PROTOCOL));
}
static void change_media(struct fake_disk *d) {
  switch (mutation) {
    case 0: d->media.MediaId++; break;
    case 1: d->media.MediaPresent = FALSE; break;
    case 2: d->media.LastBlock = 0; break;
    case 3: d->media.BlockSize = 4096; break;
    case 4: d->block.Media = NULL; break;
    case 5: d->replacement = d->media; d->block.Media = &d->replacement; break;
    case 6: d->media.LogicalPartition = TRUE; break;
    case 7: d->media.ReadOnly = FALSE; break;
    case 8: d->media.IoAlign = 4; break;
    default: abort();
  }
}
static EFI_STATUS read_blocks(EFI_BLOCK_IO_PROTOCOL *block, UINT32 id, UINT64 lba,
                              UINTN size, VOID *buffer) {
  UINTN i, test = total_reads;
  struct fake_disk *d = NULL;
  UINT8 *bytes = buffer;
  for (i = 0; i < disk_count; i++) if (block == &disks[i].block) d = &disks[i];
  assert(d != NULL && test < 3);
  assert(id == d->media.MediaId && size == 512 && lba == expected_lbas[test]);
  assert(lba <= d->media.LastBlock && d->media.MediaPresent && !d->media.LogicalPartition);
  assert(d->media.IoAlign <= 1 || (uintptr_t)buffer % d->media.IoAlign == 0);
  d->reads++;
  total_reads++;
  if (problem_read == total_reads && behavior == ERROR) return read_status;
  if (problem_read == total_reads && behavior == NO_COPY) return EFI_SUCCESS;
  if (problem_read == total_reads && behavior == PARTIAL) {
    for (i = 0; i < size; i++) if (i != missing_byte) bytes[i] = mSdSectorReferences[test][i];
    return EFI_SUCCESS;
  }
  memcpy(buffer, mSdSectorReferences[test], size);
  if (problem_read == total_reads) {
    if (behavior == MISMATCH) bytes[changed_byte] ^= 1;
    if (behavior == ZERO_FILL) memset(buffer, 0, size);
    if (behavior == MEDIA_CHANGE) change_media(d);
  }
  return EFI_SUCCESS;
}
static EFI_STATUS locate_handles(EFI_LOCATE_SEARCH_TYPE type, EFI_GUID *guid, VOID *key,
                                  UINTN *count, EFI_HANDLE **handles) {
  UINTN i;
  assert(type == ByProtocol && key == NULL);
  assert(memcmp(guid, &gEfiBlockIoProtocolGuid, sizeof(*guid)) == 0);
  if (EFI_ERROR(locate_status)) return locate_status;
  *count = zero_count ? 0 : disk_count;
  if (null_handles || disk_count == 0) return EFI_SUCCESS;
  assert(allocated_handles == NULL);
  allocated_handles = malloc(disk_count * sizeof(*allocated_handles));
  assert(allocated_handles != NULL);
  for (i = 0; i < disk_count; i++) allocated_handles[i] = &disks[i];
  *handles = allocated_handles;
  return EFI_SUCCESS;
}
static EFI_STATUS handle_protocol(EFI_HANDLE handle, EFI_GUID *guid, VOID **interface) {
  struct fake_disk *d = handle;
  if (memcmp(guid, &gEfiDevicePathProtocolGuid, sizeof(*guid)) == 0) {
    if (!EFI_ERROR(d->path_status) && !d->null_path) *interface = d->path;
    return d->path_status;
  }
  assert(memcmp(guid, &gEfiBlockIoProtocolGuid, sizeof(*guid)) == 0);
  if (!EFI_ERROR(d->block_status) && !d->null_block) *interface = &d->block;
  return d->block_status;
}
static EFI_BOOT_SERVICES services = { locate_handles, handle_protocol };
EFI_BOOT_SERVICES *gBS = &services;

static void reset_case(void) {
  UINTN i;
  assert(allocated_handles == NULL);
  memset(disks, 0, sizeof(disks));
  disk_count = 1;
  total_reads = pass_sectors = pass_all = frees = 0;
  locate_status = EFI_SUCCESS;
  read_status = EFI_DEVICE_ERROR;
  null_handles = zero_count = FALSE;
  problem_read = 1;
  missing_byte = changed_byte = 0;
  behavior = EXACT;
  mutation = 0;
  for (i = 0; i < MAX_DISKS; i++) {
    disks[i].media.MediaId = 17;
    disks[i].media.MediaPresent = TRUE;
    disks[i].media.ReadOnly = TRUE;
    disks[i].media.BlockSize = 512;
    disks[i].media.LastBlock = 60456959;
    disks[i].media.IoAlign = 8;
    disks[i].block.Media = &disks[i].media;
    disks[i].block.ReadBlocks = read_blocks;
    make_path(&disks[i], FALSE, 0, FALSE);
  }
}
static void run_case(UINTN reads, UINTN sectors, UINTN complete) {
  UINTN expected_frees = (!EFI_ERROR(locate_status) && !null_handles && disk_count != 0) ? 1u : 0u;
  assert(MotoG20RawSdProbe() == (complete != 0));
  assert(total_reads == reads && pass_sectors == sectors && pass_all == complete);
  assert(allocated_handles == NULL && frees == expected_frees);
  cases++;
}
int main(void) {
  UINTN test, byte, align;
  unsigned mut, kind;
  const UINT32 supported_alignment[] = {0, 1, 2, 4, 8};
  const UINT32 bad_alignment[] = {3, 5, 6, 7, 9, 16, UINT32_MAX};
  const UINT64 bad_last_block[] = {0, 2047, 99071, 60456958, 60456960, UINT64_MAX};
  for (align = 0; align < sizeof(supported_alignment)/sizeof(supported_alignment[0]); align++) {
    reset_case(); disks[0].media.IoAlign = supported_alignment[align]; run_case(3, 3, 1);
  }
  reset_case(); locate_status = EFI_NOT_FOUND; run_case(0, 0, 0);
  reset_case(); disk_count = 0; run_case(0, 0, 0);
  reset_case(); null_handles = TRUE; run_case(0, 0, 0);
  reset_case(); zero_count = TRUE; run_case(0, 0, 0);
  reset_case(); disks[0].path_status = EFI_NOT_FOUND; run_case(0, 0, 0);
  reset_case(); disks[0].null_path = TRUE; run_case(0, 0, 0);
  reset_case(); disks[0].block_status = EFI_NOT_FOUND; run_case(0, 0, 0);
  reset_case(); disks[0].null_block = TRUE; run_case(0, 0, 0);
  reset_case(); disks[0].block.Media = NULL; run_case(0, 0, 0);
  reset_case(); disks[0].block.ReadBlocks = NULL; run_case(0, 0, 0);
  reset_case(); disks[0].media.MediaPresent = FALSE; run_case(0, 0, 0);
  reset_case(); disks[0].media.ReadOnly = FALSE; run_case(0, 0, 0);
  reset_case(); disks[0].media.LogicalPartition = TRUE; run_case(0, 0, 0);
  reset_case(); disks[0].media.BlockSize = 4096; run_case(0, 0, 0);
  reset_case(); disks[0].media.BlockSize = 0; run_case(0, 0, 0);
  for (align = 0; align < sizeof(bad_alignment)/sizeof(bad_alignment[0]); align++) {
    reset_case(); disks[0].media.IoAlign = bad_alignment[align]; run_case(0, 0, 0);
  }
  for (test = 0; test < sizeof(bad_last_block)/sizeof(bad_last_block[0]); test++) {
    reset_case(); disks[0].media.LastBlock = bad_last_block[test]; run_case(0, 0, 0);
  }
  reset_case(); make_path(&disks[0], TRUE, 0, FALSE); run_case(0, 0, 0);
  reset_case(); make_path(&disks[0], FALSE, 1, FALSE); run_case(0, 0, 0);
  reset_case(); make_path(&disks[0], FALSE, 0, TRUE); run_case(0, 0, 0);
  reset_case(); disks[0].path[0] = MESSAGING_DEVICE_PATH; run_case(0, 0, 0);
  reset_case(); disks[0].path[sizeof(VENDOR_DEVICE_PATH) + 1] = 1; run_case(0, 0, 0);
  reset_case(); disks[0].path[2] = 0; run_case(0, 0, 0);
  reset_case(); disks[0].path[2] = 127; run_case(0, 0, 0);
  reset_case(); disks[0].path_size--; run_case(0, 0, 0);
  reset_case(); disks[0].path[disks[0].path_size - 3] = END_INSTANCE_DEVICE_PATH_SUBTYPE;
  append_node(&disks[0], END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4); run_case(0, 0, 0);
  reset_case(); disk_count = 2; make_path(&disks[0], TRUE, 0, FALSE); run_case(3, 3, 1);
  assert(disks[0].reads == 0 && disks[1].reads == 3);
  reset_case(); disk_count = 2; make_path(&disks[1], TRUE, 0, FALSE); run_case(3, 3, 1);
  assert(disks[1].reads == 0);
  for (test = 1; test <= 3; test++) {
    for (kind = ERROR; kind <= ZERO_FILL; kind++) {
      if (kind == MEDIA_CHANGE || kind == PARTIAL || kind == MISMATCH) continue;
      reset_case(); problem_read = test; behavior = kind; run_case(test, test - 1, 0);
    }
    reset_case(); problem_read = test; behavior = ERROR; read_status = 1; run_case(test, test - 1, 0);
    for (mut = 0; mut < 9; mut++) {
      reset_case(); problem_read = test; behavior = MEDIA_CHANGE; mutation = mut;
      run_case(test, test - 1, 0);
    }
    /* Every byte in every sector must be copied and match, including zero tails. */
    for (byte = 0; byte < 512; byte++) {
      reset_case(); problem_read = test; behavior = PARTIAL; missing_byte = byte;
      run_case(test, test - 1, 0);
      reset_case(); problem_read = test; behavior = MISMATCH; changed_byte = byte;
      run_case(test, test - 1, 0);
    }
  }
  printf("raw SD production probe: %zu cases passed (ASan/UBSan runner); no physical-device claim.\n", cases);
  return 0;
}
