#include "boot/uefi/uefi.h"

#define PATH_MAX 128
#define CHUNK (1 << 20)

static const struct efi_guid loaded_image_guid = EFI_LOADED_IMAGE_GUID;
static const struct efi_guid simple_fs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID;
static const struct efi_guid file_info_guid = EFI_FILE_INFO_GUID;

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

struct efi_file *file_open(const char *path) {
  efi_char wide[PATH_MAX];
  struct efi_file *f;
  uint32 n = 0;

  if (root == 0)
    return 0;
  for (; *path && n < PATH_MAX - 1; path++)
    wide[n++] = (efi_char)(*path == '/' ? '\\' : (uint8)*path);
  wide[n] = 0;
  if (EFI_ERROR(root->open(root, &f, wide, EFI_FILE_MODE_READ, 0)))
    return 0;
  return f;
}

uint64 file_length(struct efi_file *f) {
  uint8 buf[sizeof(struct efi_file_info) + PATH_MAX * sizeof(efi_char)];
  efi_uintn n = sizeof(buf);

  if (EFI_ERROR(f->get_info(f, &file_info_guid, &n, buf)))
    return 0;
  return ((const struct efi_file_info *)buf)->file_size;
}

// firmware is free to return less than asked for, so keep asking
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
