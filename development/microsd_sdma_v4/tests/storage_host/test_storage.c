/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Exercise the production connection helper using observable firmware services.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uefi_mock.h>
#include "../../bds/MotoG20StorageConnect.c"

enum { LOADED, BINDING, BLOCK, PATH, DISK, FS, COLLATION, PROTOCOLS };
#define MAX_OBJECTS 140u
struct object {
  EFI_LOADED_IMAGE_PROTOCOL loaded;
  EFI_DRIVER_BINDING_PROTOCOL binding;
  EFI_BLOCK_IO_PROTOCOL block;
  EFI_BLOCK_IO_MEDIA media;
  UINT8 path[256];
  UINTN path_size;
  EFI_STATUS status[PROTOCOLS];
  BOOLEAN null_interface[PROTOCOLS], active, child;
};
static struct object images[MAX_OBJECTS], bindings[MAX_OBJECTS], blocks[MAX_OBJECTS];
static UINTN image_count, binding_count, block_count;
static EFI_STATUS locate_status[PROTOCOLS], root_status, child_status;
static BOOLEAN null_handles[PROTOCOLS], attach_fs, change_root, change_media_root, change_media_child;
static INTN count_override[PROTOCOLS];
static UINTN locate_calls[PROTOCOLS], connects, root_connects, child_connects, frees, allocations;
static EFI_HANDLE connected[16];
static unsigned cases;
EFI_GUID gEfiLoadedImageProtocolGuid = {LOADED,0,0,{0}};
EFI_GUID gEfiDriverBindingProtocolGuid = {BINDING,0,0,{0}};
EFI_GUID gEfiBlockIoProtocolGuid = {BLOCK,0,0,{0}};
EFI_GUID gEfiDevicePathProtocolGuid = {PATH,0,0,{0}};
EFI_GUID gEfiDiskIoProtocolGuid = {DISK,0,0,{0}};
EFI_GUID gEfiSimpleFileSystemProtocolGuid = {FS,0,0,{0}};
EFI_GUID gEfiUnicodeCollation2ProtocolGuid = {COLLATION,0,0,{0}};
static CONST EFI_GUID file_guids[3] = {
  {0x6b38f7b4,0xad98,0x40e9,{0x90,0x93,0xac,0xa2,0xb5,0xa2,0x53,0xc4}},
  {0x1fa1f39e,0xfeff,0x4aae,{0xbd,0x7b,0x38,0xa0,0x70,0xa3,0xb6,0x09}},
  {0x961578fe,0xb6b7,0x44c3,{0xaf,0x35,0x6b,0xc7,0x05,0xcd,0x2b,0x1f}}
};

INTN CompareMem (CONST VOID *a, CONST VOID *b, UINTN n) { return memcmp(a,b,n); }
VOID FreePool (VOID *p) { assert(p != NULL); free(p); frees++; }
UINTN Print (CONST CHAR16 *format, ...) { assert(format != NULL); return 0; }

/* Bound the fake validator to each real allocation, unlike an unbounded walk.
 * Node-specific filtering remains exclusively in the production helper. */
BOOLEAN IsDevicePathValid (CONST EFI_DEVICE_PATH_PROTOCOL *path, UINTN maximum) {
  UINTN size = 0, offset = 0, i;
  CONST UINT8 *p = (CONST UINT8 *)path;
  for (i = 0; i < MAX_OBJECTS; i++) {
    if (p == images[i].path) size = images[i].path_size;
    if (p == blocks[i].path) size = blocks[i].path_size;
  }
  if (size == 0 || size > maximum) return FALSE;
  while (offset + sizeof(*path) <= size) {
    CONST EFI_DEVICE_PATH_PROTOCOL *node = (CONST EFI_DEVICE_PATH_PROTOCOL *)(p + offset);
    UINTN n = DevicePathNodeLength(node);
    if (n < sizeof(*node) || n > size - offset) return FALSE;
    if (IsDevicePathEnd(node)) return n == sizeof(*node) && offset + n == size;
    offset += n;
  }
  return FALSE;
}

static struct object *checked_object(EFI_HANDLE handle) {
  UINTN i;
  for (i = 0; i < MAX_OBJECTS; i++) {
    if (handle == &images[i]) return &images[i];
    if (handle == &bindings[i]) return &bindings[i];
    if (handle == &blocks[i]) return &blocks[i];
  }
  abort();
}

static EFI_STATUS handle_protocol(EFI_HANDLE handle, EFI_GUID *guid, VOID **out) {
  struct object *o = checked_object(handle);
  UINTN p = guid->Data1;
  assert(p < PROTOCOLS);
  *out = NULL;
  if (EFI_ERROR(o->status[p]) || o->null_interface[p]) return o->status[p];
  switch (p) {
    case LOADED: *out = &o->loaded; break;
    case BINDING: *out = &o->binding; break;
    case BLOCK: *out = &o->block; break;
    case PATH: *out = o->path; break;
    default: *out = o; break;
  }
  return o->status[p];
}

static EFI_STATUS locate_protocol(EFI_GUID *guid, VOID *registration, VOID **out) {
  assert(guid == &gEfiUnicodeCollation2ProtocolGuid && registration == NULL);
  *out = NULL;
  return EFI_NOT_FOUND; /* A diagnostic observation, not a claim FAT can bind. */
}

static EFI_STATUS locate_handles(EFI_LOCATE_SEARCH_TYPE type, EFI_GUID *guid, VOID *key,
                                 UINTN *count, EFI_HANDLE **out) {
  UINTN p = guid->Data1, n = 0, i;
  struct object *objects;
  assert(type == ByProtocol && key == NULL && p < PROTOCOLS);
  locate_calls[p]++;
  *count = 0;
  *out = NULL;
  if (EFI_ERROR(locate_status[p])) return locate_status[p];
  if (p == LOADED) { objects = images; n = image_count; }
  else if (p == BINDING) { objects = bindings; n = binding_count; }
  else { assert(p == BLOCK); objects = blocks; n = block_count; }
  for (i = 0; i < n; i++) if (objects[i].active) (*count)++;
  if (count_override[p] >= 0) *count = (UINTN)count_override[p];
  if (null_handles[p]) return EFI_SUCCESS;
  if (*count == 0) return EFI_NOT_FOUND;
  *out = calloc(*count, sizeof(**out));
  assert(*out != NULL);
  allocations++;
  if (count_override[p] >= 0) {
    for (i = 0; i < *count; i++) (*out)[i] = &objects[0];
  } else {
    UINTN j = 0;
    for (i = 0; i < n; i++) if (objects[i].active) (*out)[j++] = &objects[i];
  }
  return EFI_SUCCESS;
}

static EFI_STATUS connect_controller(EFI_HANDLE handle, EFI_HANDLE *priority,
                                     EFI_DEVICE_PATH_PROTOCOL *remaining, BOOLEAN recursive) {
  struct object *o = checked_object(handle);
  UINTN i;
  assert(remaining == NULL && !recursive && connects < 16);
  assert(priority[0] == &images[0]);
  connected[connects++] = handle;
  if (!o->child) {
    assert(handle == &blocks[0]);
    assert(priority[1] == &images[1] && priority[2] == &images[2] && priority[3] == NULL);
    root_connects++;
    if (!EFI_ERROR(root_status) || root_status == EFI_NOT_FOUND) {
      for (i = 1; i < block_count; i++) blocks[i].active = TRUE;
      blocks[0].status[DISK] = EFI_SUCCESS;
      if (change_media_root) blocks[0].media.MediaId++;
      if (change_root) blocks[0].active = FALSE;
    }
    return root_status;
  }
  assert(priority[1] == &images[2] && priority[2] == NULL);
  child_connects++;
  if ((!EFI_ERROR(child_status) || child_status == EFI_NOT_FOUND) && attach_fs) {
    o->status[DISK] = o->status[FS] = EFI_SUCCESS;
  }
  if (change_media_child) blocks[0].media.MediaId++;
  return child_status;
}
static EFI_BOOT_SERVICES services = {locate_protocol, locate_handles, handle_protocol, connect_controller};
EFI_BOOT_SERVICES *gBS = &services;

static VOID header(EFI_DEVICE_PATH_PROTOCOL *p, UINT8 type, UINT8 subtype, UINTN size) {
  assert(size <= 255);
  p->Type = type;
  p->SubType = subtype;
  p->Length[0] = (UINT8)size;
  p->Length[1] = 0;
}
static VOID defaults(struct object *o) {
  UINTN p;
  memset(o, 0, sizeof(*o));
  for (p = 0; p < PROTOCOLS; p++) o->status[p] = EFI_UNSUPPORTED;
  o->active = TRUE;
}
static VOID make_image(UINTN i, UINTN which) {
  struct object *o = &images[i];
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH file;
  EFI_DEVICE_PATH_PROTOCOL end;
  defaults(o);
  header(&file.Header, MEDIA_DEVICE_PATH, MEDIA_PIWG_FW_FILE_DP, sizeof(file));
  file.FvFileName = file_guids[which];
  header(&end, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(end));
  memcpy(o->path, &file, sizeof(file));
  memcpy(o->path + sizeof(file), &end, sizeof(end));
  o->path_size = sizeof(file) + sizeof(end);
  o->loaded.FilePath = (EFI_DEVICE_PATH_PROTOCOL *)o->path;
  o->status[LOADED] = EFI_SUCCESS;
}
static VOID make_binding(UINTN i, UINTN image) {
  struct object *o = &bindings[i];
  defaults(o);
  o->binding.ImageHandle = &images[image];
  o->binding.DriverBindingHandle = o;
  o->status[BINDING] = EFI_SUCCESS;
}
static VOID make_block(UINTN i, BOOLEAN child) {
  struct object *o = &blocks[i];
  VENDOR_DEVICE_PATH vendor;
  SD_DEVICE_PATH sd;
  HARDDRIVE_DEVICE_PATH hd;
  EFI_DEVICE_PATH_PROTOCOL end;
  defaults(o);
  o->child = child;
  o->block.Media = &o->media;
  o->media.MediaId = 42;
  o->media.MediaPresent = TRUE;
  o->media.LogicalPartition = child;
  o->media.BlockSize = 512;
  o->media.LastBlock = child ? 4095 : 60456959;
  o->status[PATH] = o->status[BLOCK] = EFI_SUCCESS;
  header(&vendor.Header, HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof(vendor));
  vendor.Guid = mSdController;
  header(&sd.Header, MESSAGING_DEVICE_PATH, MSG_SD_DP, sizeof(sd));
  sd.SlotNumber = 0;
  memcpy(o->path, &vendor, sizeof(vendor));
  memcpy(o->path + sizeof(vendor), &sd, sizeof(sd));
  o->path_size = sizeof(vendor) + sizeof(sd);
  if (child) {
    memset(&hd, 0, sizeof(hd));
    header(&hd.Header, MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP, sizeof(hd));
    hd.PartitionNumber = (UINT32)i + 1;
    hd.PartitionStart = 2048;
    hd.PartitionSize = 4096;
    memcpy(o->path + o->path_size, &hd, sizeof(hd));
    o->path_size += sizeof(hd);
  }
  header(&end, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(end));
  memcpy(o->path + o->path_size, &end, sizeof(end));
  o->path_size += sizeof(end);
}
static VOID reset(VOID) {
  UINTN i;
  assert(allocations == frees);
  memset(images, 0, sizeof(images));
  memset(bindings, 0, sizeof(bindings));
  memset(blocks, 0, sizeof(blocks));
  memset(locate_status, 0, sizeof(locate_status));
  memset(null_handles, 0, sizeof(null_handles));
  memset(locate_calls, 0, sizeof(locate_calls));
  memset(connected, 0, sizeof(connected));
  for (i = 0; i < PROTOCOLS; i++) count_override[i] = -1;
  for (i = 0; i < 3; i++) { make_image(i,i); make_binding(i,i); }
  make_block(0,FALSE);
  make_block(1,TRUE);
  blocks[1].active = FALSE; /* Created by the root connection by default. */
  image_count = binding_count = 3;
  block_count = 2;
  connects = root_connects = child_connects = allocations = frees = 0;
  root_status = child_status = EFI_SUCCESS;
  change_root = change_media_root = change_media_child = FALSE;
  attach_fs = TRUE;
  mStorageAttempted = FALSE;
}
static VOID expect(CONST char *name, EFI_STATUS status, UINTN calls) {
  EFI_STATUS actual = MotoG20StorageConnect();
  if (actual != status || connects != calls) {
    fprintf(stderr, "%s: status=%llx expected=%llx calls=%zu expected=%zu\n",
            name, (unsigned long long)actual, (unsigned long long)status, connects, calls);
    abort();
  }
  assert(allocations == frees);
  cases++;
  printf("PASS %s\n",name);
}
static EFI_DEVICE_PATH_PROTOCOL *block_node(UINTN i, UINTN offset) {
  return (EFI_DEVICE_PATH_PROTOCOL *)(blocks[i].path + offset);
}
static VOID append_before_end(struct object *o, CONST VOID *data, UINTN size) {
  UINTN end = o->path_size - sizeof(EFI_DEVICE_PATH_PROTOCOL);
  assert(o->path_size + size <= sizeof(o->path));
  memmove(o->path + end + size, o->path + end, sizeof(EFI_DEVICE_PATH_PROTOCOL));
  memcpy(o->path + end, data, size);
  o->path_size += size;
}

int main(void) {
  UINTN i, p;
  EFI_DEVICE_PATH_PROTOCOL extra;
  UINT32 seed = 0x67532026;
  reset();
  expect("partition created after root; ordered ImageHandles; present FS",EFI_SUCCESS,2);
  assert(root_connects == 1 && child_connects == 1 && locate_calls[BLOCK] == 3);
  expect("one bounded pass only",EFI_ALREADY_STARTED,2);
  reset(); blocks[1].active = TRUE; root_status = child_status = EFI_NOT_FOUND;
  blocks[1].status[FS] = EFI_SUCCESS;
  expect("already connected FS is discoverable despite NOT_FOUND",EFI_SUCCESS,2);
  reset(); attach_fs = FALSE;
  expect("successful connections alone do not prove FS",EFI_NOT_FOUND,2);
  reset(); block_count = 1; blocks[0].status[FS] = EFI_SUCCESS;
  expect("root FS is not mistaken for partition FS",EFI_NOT_FOUND,1);
  reset(); binding_count = 2;
  expect("missing Fat binding prevents connection",EFI_NOT_READY,0);
  reset(); make_image(3,0); image_count = 4;
  expect("ambiguous image prevents connection",EFI_NOT_READY,0);
  reset(); make_binding(3,0); binding_count = 4;
  expect("ambiguous binding prevents connection",EFI_NOT_READY,0);
  reset(); bindings[2].binding.ImageHandle = &images[0];
  expect("wrong binding ImageHandle prevents connection",EFI_NOT_READY,0);
  reset(); bindings[2].binding.DriverBindingHandle = &bindings[0];
  expect("wrong binding self handle prevents connection",EFI_NOT_READY,0);
  reset(); images[1].loaded.FilePath = NULL;
  expect("missing image path prevents connection",EFI_NOT_READY,0);
  reset(); images[2].path[4] ^= 1;
  expect("unrelated FV image ignored",EFI_NOT_READY,0);
  reset(); {
    VENDOR_DEVICE_PATH prefix;
    header(&prefix.Header,HARDWARE_DEVICE_PATH,HW_VENDOR_DP,sizeof(prefix));
    prefix.Guid=mSdController;
    memmove(images[0].path+sizeof(prefix),images[0].path,images[0].path_size);
    memcpy(images[0].path,&prefix,sizeof(prefix));
    images[0].path_size+=sizeof(prefix);
  }
  expect("FV filename matched after a valid leading path",EFI_SUCCESS,2);
  reset(); header(&extra,MEDIA_DEVICE_PATH,4,sizeof(extra));
  append_before_end(&images[2],&extra,sizeof(extra));
  expect("FV filename must be final non-end node",EFI_NOT_READY,0);
  reset(); header(&extra,END_DEVICE_PATH_TYPE,END_INSTANCE_DEVICE_PATH_SUBTYPE,sizeof(extra));
  append_before_end(&images[2],&extra,sizeof(extra));
  expect("image multi-instance path rejected",EFI_NOT_READY,0);
  reset(); images[1].path[2] = 3;
  expect("malformed image node rejected",EFI_NOT_READY,0);
  reset(); block_count = 0;
  expect("no BlockIo root",EFI_NOT_FOUND,0);
  reset(); make_block(1,FALSE);
  expect("multiple physical roots rejected",EFI_UNSUPPORTED,0);
  reset(); blocks[0].media.MediaPresent = FALSE;
  expect("absent root media prevents connection",EFI_NO_MEDIA,0);
  reset(); blocks[0].media.BlockSize = 0;
  expect("zero root block size rejected",EFI_DEVICE_ERROR,0);
  reset(); blocks[0].media.LogicalPartition = TRUE;
  expect("logical root mismatch rejected",EFI_NOT_FOUND,0);
  reset(); blocks[1].media.LogicalPartition = FALSE;
  expect("physical partition mismatch excluded",EFI_NOT_FOUND,1);
  reset(); blocks[1].media.MediaPresent = FALSE;
  expect("absent partition excluded",EFI_NOT_FOUND,1);
  reset(); blocks[1].media.BlockSize = 0;
  expect("zero partition block size rejected after root",EFI_DEVICE_ERROR,1);
  reset(); blocks[0].path[4] ^= 1;
  expect("unrelated physical GUID excluded",EFI_NOT_FOUND,0);
  reset(); ((SD_DEVICE_PATH *)block_node(0,sizeof(VENDOR_DEVICE_PATH)))->SlotNumber = 1;
  expect("unrelated SD slot excluded",EFI_NOT_FOUND,0);
  reset(); blocks[0].path[2] = 3;
  expect("malformed physical node excluded",EFI_NOT_FOUND,0);
  reset(); header(&extra,MEDIA_DEVICE_PATH,4,sizeof(extra));
  append_before_end(&blocks[0],&extra,sizeof(extra));
  expect("extra physical path node excluded",EFI_NOT_FOUND,0);
  reset(); header(&extra,END_DEVICE_PATH_TYPE,END_INSTANCE_DEVICE_PATH_SUBTYPE,sizeof(extra));
  append_before_end(&blocks[0],&extra,sizeof(extra));
  expect("physical multi-instance path excluded",EFI_NOT_FOUND,0);
  reset(); ((HARDDRIVE_DEVICE_PATH *)block_node(1,sizeof(VENDOR_DEVICE_PATH)+sizeof(SD_DEVICE_PATH)))->PartitionNumber = 0;
  expect("zero partition number excluded",EFI_NOT_FOUND,1);
  reset(); ((HARDDRIVE_DEVICE_PATH *)block_node(1,sizeof(VENDOR_DEVICE_PATH)+sizeof(SD_DEVICE_PATH)))->PartitionSize = 0;
  expect("zero partition size excluded",EFI_NOT_FOUND,1);
  reset(); append_before_end(&blocks[1],blocks[1].path+sizeof(VENDOR_DEVICE_PATH)+sizeof(SD_DEVICE_PATH),sizeof(HARDDRIVE_DEVICE_PATH));
  expect("nested partition path excluded",EFI_NOT_FOUND,1);
  reset(); for (i=0;i<9;i++) make_block(i,TRUE); block_count=9;
  expect("nine children without root cannot overflow eight slots",EFI_BAD_BUFFER_SIZE,0);
  reset(); for (i=1;i<9;i++) make_block(i,TRUE); block_count=9;
  expect("eight children accepted with one root",EFI_SUCCESS,9);
  reset(); for (i=1;i<10;i++) make_block(i,TRUE); block_count=10;
  expect("nine children rejected with root",EFI_BAD_BUFFER_SIZE,0);
  for (p=LOADED;p<=BLOCK;p++) {
    reset(); locate_status[p]=EFI_DEVICE_ERROR;
    expect("LocateHandleBuffer failure preserved",EFI_DEVICE_ERROR,0);
    reset(); null_handles[p]=TRUE;
    expect("null handle array rejected",EFI_BAD_BUFFER_SIZE,0);
    reset(); count_override[p]=(p==BLOCK ? 65 : 129);
    expect("handle count cap enforced before traversal",EFI_BAD_BUFFER_SIZE,0);
  }
  reset(); images[1].status[LOADED]=EFI_DEVICE_ERROR;
  expect("LoadedImage query error preserved",EFI_DEVICE_ERROR,0);
  reset(); images[1].null_interface[LOADED]=TRUE;
  expect("LoadedImage success with null rejected",EFI_DEVICE_ERROR,0);
  reset(); bindings[1].status[BINDING]=EFI_DEVICE_ERROR;
  expect("DriverBinding query error preserved",EFI_DEVICE_ERROR,0);
  reset(); bindings[1].null_interface[BINDING]=TRUE;
  expect("DriverBinding success with null rejected",EFI_DEVICE_ERROR,0);
  reset(); blocks[0].status[PATH]=EFI_DEVICE_ERROR;
  expect("DevicePath query error preserved",EFI_DEVICE_ERROR,0);
  reset(); blocks[0].status[PATH]=EFI_UNSUPPORTED;
  expect("absent DevicePath excluded",EFI_NOT_FOUND,0);
  reset(); blocks[0].null_interface[PATH]=TRUE;
  expect("DevicePath success with null rejected",EFI_DEVICE_ERROR,0);
  reset(); blocks[0].status[BLOCK]=EFI_DEVICE_ERROR;
  expect("BlockIo query error preserved",EFI_DEVICE_ERROR,0);
  reset(); blocks[0].null_interface[BLOCK]=TRUE;
  expect("BlockIo success with null rejected",EFI_DEVICE_ERROR,0);
  reset(); blocks[0].block.Media=NULL;
  expect("null Media rejected",EFI_DEVICE_ERROR,0);
  reset(); blocks[0].status[DISK]=EFI_DEVICE_ERROR;
  expect("DiskIo query error preserved",EFI_DEVICE_ERROR,0);
  reset(); blocks[0].status[DISK]=EFI_SUCCESS; blocks[0].null_interface[DISK]=TRUE;
  expect("DiskIo success with null rejected",EFI_DEVICE_ERROR,0);
  reset(); blocks[1].status[FS]=EFI_DEVICE_ERROR;
  expect("SFS query error preserved after root",EFI_DEVICE_ERROR,1);
  reset(); blocks[1].status[FS]=EFI_SUCCESS; blocks[1].null_interface[FS]=TRUE;
  expect("SFS success with null rejected",EFI_DEVICE_ERROR,1);
  reset(); root_status=EFI_DEVICE_ERROR;
  expect("root connection error cannot become success",EFI_DEVICE_ERROR,1);
  reset(); child_status=EFI_DEVICE_ERROR;
  expect("child connection error cannot become success",EFI_DEVICE_ERROR,2);
  reset(); change_media_root=TRUE;
  expect("root media generation change detected",EFI_MEDIA_CHANGED,1);
  reset(); change_media_child=TRUE;
  expect("child connection media generation change detected",EFI_MEDIA_CHANGED,2);
  reset(); change_root=TRUE; make_block(2,FALSE); blocks[2].active=FALSE; block_count=3;
  expect("replacement root handle detected",EFI_MEDIA_CHANGED,1);
  reset(); make_block(2,FALSE); blocks[2].path[4]^=1; block_count=3;
  expect("unrelated BlockIo never connected",EFI_SUCCESS,2);
  assert(connected[0]==&blocks[0] && connected[1]==&blocks[1]);
  /* Deterministic malformed path corpus. The actual production matchers walk
   * these allocations under ASan/UBSan, with no firmware or hardware claim. */
  for (i=0;i<10000;i++) {
    UINTN j, size;
    reset();
    seed=seed*UINT32_C(1664525)+UINT32_C(1013904223);
    size=(UINTN)(seed%256u);
    images[0].path_size=blocks[0].path_size=size;
    for (j=0;j<size;j++) {
      seed=seed*UINT32_C(1664525)+UINT32_C(1013904223);
      images[0].path[j]=blocks[0].path[j]=(UINT8)(seed>>24);
    }
    assert(StoragePathKind((EFI_DEVICE_PATH_PROTOCOL *)blocks[0].path)==0);
    assert(!StorageImageMatches((EFI_DEVICE_PATH_PROTOCOL *)images[0].path,&file_guids[0]));
  }
  printf("PASS %u behavioral cases + 10000 malformed-path pairs; ASan/UBSan host-only\n",cases);
  return 0;
}
