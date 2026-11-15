#include "boot/uefi/uefi.h"

#define PATHLEN 128
#define CHUNK (1 << 20)
#define DP_MAX 1024

static const struct efi_guid loaded_image_guid = EFI_LOADED_IMAGE_GUID;
static const struct efi_guid simple_fs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID;
static const struct efi_guid file_info_guid = EFI_FILE_INFO_GUID;
static const struct efi_guid device_path_guid = EFI_DEVICE_PATH_GUID;
static const struct efi_guid block_io_guid = EFI_BLOCK_IO_GUID;

static struct efi_file *root;

int file_mount(void) {
  struct efi_loaded_image *li;
  struct efi_simple_file_system *fs;

  if (EFI_ERROR(BS->handle_protocol(IMAGE, &loaded_image_guid, (void **)&li)))
    return -1;
  if (EFI_ERROR(BS->handle_protocol(li->device_handle, &simple_fs_guid,
                                    (void **)&fs)))
    return -1;
  if (EFI_ERROR(fs->open_volume(fs, &root)))
    return -1;
  return 0;
}

static uint32 dp_len(const struct efi_device_path *dp) {
  return (uint32)dp->length[0] | ((uint32)dp->length[1] << 8);
}

static struct efi_device_path *image_partition(struct efi_device_path **path) {
  struct efi_loaded_image *li;
  struct efi_device_path *dp;
  uint32 len, total = 0;

  if (EFI_ERROR(BS->handle_protocol(IMAGE, &loaded_image_guid, (void **)&li)) ||
      EFI_ERROR(BS->handle_protocol(li->device_handle, &device_path_guid,
                                    (void **)&dp)))
    return 0;
  *path = dp;
  for (; dp->type != EFI_DP_END; dp = (struct efi_device_path *)((uint8 *)dp + len)) {
    if ((len = dp_len(dp)) < sizeof(*dp) || (total += len) > DP_MAX)
      return 0;
    if (dp->type == EFI_DP_MEDIA && dp->subtype == EFI_DP_HARDDRIVE)
      return dp;
  }
  return 0;
}

int file_partition_guid(uint8 out[16]) {
  struct efi_device_path *path, *hd = image_partition(&path);

  if (hd == 0 || dp_len(hd) <= EFI_DP_HD_SIGTYPE ||
      ((uint8 *)hd)[EFI_DP_HD_SIGTYPE] != EFI_DP_SIG_GUID)
    return -1;
  memcpy(out, (uint8 *)hd + EFI_DP_HD_SIG, 16);
  return 0;
}

static uint8 *disk_bounce;
static uint32 disk_shift;

static int disk_read(void *ctx, uint64 lba, uint32 count, void *dst) {
  struct efi_block_io *io = ctx;
  uint32 shift = disk_shift, per = EFI_PAGE_SIZE >> shift, n;
  uint8 *out = dst;

  while (count > 0) {
    n = count > per ? per : count;
    if (EFI_ERROR(io->read_blocks(io, io->media->media_id, lba,
                                  (efi_uintn)n << shift, disk_bounce)))
      return -1;
    memcpy(out, disk_bounce, (uint64)n << shift);
    out += (uint64)n << shift;
    lba += n;
    count -= n;
  }
  return 0;
}

int file_bootdisk(struct bootdisk *d) {
  static uint8 whole[DP_MAX + sizeof(struct efi_device_path)];
  struct efi_device_path *path, *hd = image_partition(&path), *rest, *end;
  struct efi_block_io *io;
  struct efi_block_io_media *m;
  efi_handle disk;
  efi_phys page;
  uint32 head, bsize;

  if (hd == 0)
    return -1;
  head = (uint32)((uint8 *)hd - (uint8 *)path);
  memcpy(whole, path, head);
  end = (struct efi_device_path *)(whole + head);
  end->type = EFI_DP_END;
  end->subtype = EFI_DP_END_ENTIRE;
  end->length[0] = sizeof(*end);
  end->length[1] = 0;
  rest = (struct efi_device_path *)whole;
  if (EFI_ERROR(BS->locate_device_path(&block_io_guid, &rest, &disk)) ||
      rest->type != EFI_DP_END ||
      EFI_ERROR(BS->handle_protocol(disk, &block_io_guid, (void **)&io)))
    return -1;
  m = io->media;
  bsize = m->block_size;
  if (m->logical_partition || !m->media_present || bsize < 512 ||
      bsize > EFI_PAGE_SIZE || (bsize & (bsize - 1)) != 0 ||
      m->io_align > EFI_PAGE_SIZE)
    return -1;
  if (EFI_ERROR(BS->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, 1,
                                   &page)))
    return -1;
  disk_bounce = (uint8 *)(uintptr_t)page;
  for (disk_shift = 9; (1u << disk_shift) != bsize; disk_shift++)
    ;
  d->read = disk_read;
  d->ctx = io;
  d->shift = disk_shift;
  d->blocks = m->last_block + 1;
  return 0;
}

struct efi_file *file_open(const char *path) {
  efi_char wide[PATHLEN];
  struct efi_file *f;
  uint32 n = 0;

  if (root == 0)
    return 0;
  for (; *path && n < PATHLEN - 1; path++)
    wide[n++] = (efi_char)(*path == '/' ? '\\' : (uint8)*path);
  wide[n] = 0;
  if (EFI_ERROR(root->open(root, &f, wide, EFI_FILE_MODE_READ, 0)))
    return 0;
  return f;
}

uint64 file_length(struct efi_file *f) {
  uint8 buf[sizeof(struct efi_file_info) + PATHLEN * sizeof(efi_char)];
  efi_uintn n = sizeof(buf);

  if (EFI_ERROR(f->get_info(f, &file_info_guid, &n, buf)))
    return 0;
  return ((const struct efi_file_info *)buf)->file_size;
}

int file_pread(struct efi_file *f, uint64 off, void *dst, uint64 n,
               uint64 *got) {
  uint8 *p = dst;
  uint64 done = 0;

  *got = 0;
  if (EFI_ERROR(f->set_position(f, off)))
    return -1;
  while (done < n) {
    efi_uintn want = n - done > CHUNK ? CHUNK : n - done;

    if (EFI_ERROR(f->read(f, &want, p + done)))
      return -1;
    if (want == 0)
      break;
    done += want;
  }
  *got = done;
  return 0;
}

void file_close(struct efi_file *f) {
  if (f != 0)
    f->close(f);
}
