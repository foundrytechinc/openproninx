#ifndef PRONINX_BOOT_UEFI_EFI_H
#define PRONINX_BOOT_UEFI_EFI_H

/*
 * The slice of UEFI 2.10 this loader speaks. Written against the published
 * specification, so nothing here is derived from EDK2 or gnu-efi.
 */

#include "inc/types.h"

#define EFIAPI __attribute__((ms_abi))

typedef uint64 efi_status;
typedef uint64 efi_uintn;
typedef void *efi_handle;
typedef uint64 efi_phys;
typedef uint16 efi_char;

struct efi_guid {
  uint32 a;
  uint16 b;
  uint16 c;
  uint8 d[8];
};

#define EFI_ERR(n) (0x8000000000000000ULL | (n))
#define EFI_SUCCESS 0ULL
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_ERROR(s) (((efi_status)(s) & 0x8000000000000000ULL) != 0)

// EFI_MEMORY_TYPE
#define EFI_RESERVED 0
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_RUNTIME_SERVICES_CODE 5
#define EFI_RUNTIME_SERVICES_DATA 6
#define EFI_CONVENTIONAL_MEMORY 7
#define EFI_UNUSABLE_MEMORY 8
#define EFI_ACPI_RECLAIM_MEMORY 9
#define EFI_ACPI_MEMORY_NVS 10
#define EFI_MEMORY_MAPPED_IO 11
#define EFI_MEMORY_MAPPED_IO_PORT 12
#define EFI_PAL_CODE 13
#define EFI_PERSISTENT_MEMORY 14

#define EFI_MEMORY_WB 0x0000000000000008ULL
#define EFI_MEMORY_RO 0x0000000000020000ULL
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

// EFI_ALLOCATE_TYPE
#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_ALLOCATE_ADDRESS 2

#define EFI_PAGE_SIZE 4096

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

struct efi_memory_descriptor {
  uint32 type;
  uint32 pad;
  efi_phys physical_start;
  uint64 virtual_start;
  uint64 pages;
  uint64 attribute;
};

_Static_assert(sizeof(struct efi_memory_descriptor) == 40,
               "efi_memory_descriptor");

struct efi_table_header {
  uint64 signature;
  uint32 revision;
  uint32 header_size;
  uint32 crc32;
  uint32 reserved;
};

struct efi_text_output {
  efi_status(EFIAPI *reset)(struct efi_text_output *, uint8);
  efi_status(EFIAPI *output_string)(struct efi_text_output *, efi_char *);
  efi_status(EFIAPI *test_string)(struct efi_text_output *, efi_char *);
  efi_status(EFIAPI *query_mode)(struct efi_text_output *, efi_uintn,
                                 efi_uintn *, efi_uintn *);
  efi_status(EFIAPI *set_mode)(struct efi_text_output *, efi_uintn);
  efi_status(EFIAPI *set_attribute)(struct efi_text_output *, efi_uintn);
  efi_status(EFIAPI *clear_screen)(struct efi_text_output *);
  efi_status(EFIAPI *set_cursor_position)(struct efi_text_output *, efi_uintn,
                                          efi_uintn);
  efi_status(EFIAPI *enable_cursor)(struct efi_text_output *, uint8);
  void *mode;
};

// the table is fixed by the specification; every entry stays, used or not
struct efi_boot_services {
  struct efi_table_header hdr;

  void *raise_tpl;
  void *restore_tpl;

  efi_status(EFIAPI *allocate_pages)(uint32 type, uint32 memtype,
                                     efi_uintn pages, efi_phys *memory);
  efi_status(EFIAPI *free_pages)(efi_phys memory, efi_uintn pages);
  efi_status(EFIAPI *get_memory_map)(efi_uintn *size,
                                     struct efi_memory_descriptor *map,
                                     efi_uintn *key, efi_uintn *descsize,
                                     uint32 *descver);
  efi_status(EFIAPI *allocate_pool)(uint32 memtype, efi_uintn size,
                                    void **buffer);
  efi_status(EFIAPI *free_pool)(void *buffer);

  void *create_event;
  void *set_timer;
  void *wait_for_event;
  void *signal_event;
  void *close_event;
  void *check_event;

  void *install_protocol_interface;
  void *reinstall_protocol_interface;
  void *uninstall_protocol_interface;
  efi_status(EFIAPI *handle_protocol)(efi_handle handle,
                                      const struct efi_guid *protocol,
                                      void **interface);
  void *reserved;
  void *register_protocol_notify;
  efi_status(EFIAPI *locate_handle)(uint32 search, const struct efi_guid *proto,
                                    void *key, efi_uintn *size,
                                    efi_handle *buffer);
  void *locate_device_path;
  void *install_configuration_table;

  void *load_image;
  void *start_image;
  void *exit;
  void *unload_image;
  efi_status(EFIAPI *exit_boot_services)(efi_handle image, efi_uintn key);

  void *get_next_monotonic_count;
  efi_status(EFIAPI *stall)(efi_uintn microseconds);
  efi_status(EFIAPI *set_watchdog_timer)(efi_uintn timeout, uint64 code,
                                         efi_uintn size, efi_char *data);

  void *connect_controller;
  void *disconnect_controller;

  void *open_protocol;
  void *close_protocol;
  void *open_protocol_information;

  void *protocols_per_handle;
  efi_status(EFIAPI *locate_handle_buffer)(uint32 search,
                                           const struct efi_guid *proto,
                                           void *key, efi_uintn *count,
                                           efi_handle **buffer);
  efi_status(EFIAPI *locate_protocol)(const struct efi_guid *proto,
                                      void *registration, void **interface);
  void *install_multiple_protocol_interfaces;
  void *uninstall_multiple_protocol_interfaces;

  void *calculate_crc32;

  void *copy_mem;
  void *set_mem;
  void *create_event_ex;
};

struct efi_configuration_table {
  struct efi_guid vendor_guid;
  void *vendor_table;
};

struct efi_system_table {
  struct efi_table_header hdr;
  efi_char *firmware_vendor;
  uint32 firmware_revision;
  uint32 pad;
  efi_handle console_in_handle;
  void *con_in;
  efi_handle console_out_handle;
  struct efi_text_output *con_out;
  efi_handle standard_error_handle;
  struct efi_text_output *std_err;
  void *runtime_services;
  struct efi_boot_services *boot_services;
  efi_uintn table_entries;
  struct efi_configuration_table *configuration_table;
};

#define EFI_LOADED_IMAGE_GUID                                                  \
  { 0x5b1b31a1, 0x9562, 0x11d2,                                                \
    {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

struct efi_loaded_image {
  uint32 revision;
  efi_handle parent_handle;
  struct efi_system_table *system_table;
  efi_handle device_handle;
  void *file_path;
  void *reserved;
  uint32 load_options_size;
  void *load_options;
  void *image_base;
  uint64 image_size;
  uint32 image_code_type;
  uint32 image_data_type;
  void *unload;
};

#define EFI_SIMPLE_FILE_SYSTEM_GUID                                            \
  { 0x964e5b22, 0x6459, 0x11d2,                                                \
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

#define EFI_FILE_INFO_GUID                                                     \
  { 0x09576e92, 0x6d3f, 0x11d2,                                                \
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

struct efi_file;

struct efi_simple_file_system {
  uint64 revision;
  efi_status(EFIAPI *open_volume)(struct efi_simple_file_system *,
                                  struct efi_file **);
};

struct efi_file {
  uint64 revision;
  efi_status(EFIAPI *open)(struct efi_file *, struct efi_file **, efi_char *,
                           uint64 mode, uint64 attributes);
  efi_status(EFIAPI *close)(struct efi_file *);
  efi_status(EFIAPI *delete)(struct efi_file *);
  efi_status(EFIAPI *read)(struct efi_file *, efi_uintn *size, void *buffer);
  efi_status(EFIAPI *write)(struct efi_file *, efi_uintn *size, void *buffer);
  efi_status(EFIAPI *get_position)(struct efi_file *, uint64 *);
  efi_status(EFIAPI *set_position)(struct efi_file *, uint64);
  efi_status(EFIAPI *get_info)(struct efi_file *, const struct efi_guid *,
                               efi_uintn *size, void *buffer);
  efi_status(EFIAPI *set_info)(struct efi_file *, const struct efi_guid *,
                               efi_uintn size, void *buffer);
  efi_status(EFIAPI *flush)(struct efi_file *);
};

struct efi_time {
  uint16 year;
  uint8 month;
  uint8 day;
  uint8 hour;
  uint8 minute;
  uint8 second;
  uint8 pad1;
  uint32 nanosecond;
  int16_t timezone;
  uint8 daylight;
  uint8 pad2;
};

struct efi_file_info {
  uint64 size;
  uint64 file_size;
  uint64 physical_size;
  struct efi_time create_time;
  struct efi_time last_access_time;
  struct efi_time modification_time;
  uint64 attribute;
  efi_char file_name[1];
};

_Static_assert(sizeof(struct efi_time) == 16, "efi_time");

#define EFI_GRAPHICS_OUTPUT_GUID                                               \
  { 0x9042a9de, 0x23dc, 0x4a38,                                                \
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a} }

#define EFI_EDID_ACTIVE_GUID                                                   \
  { 0xbd8c1056, 0x9f36, 0x44ec,                                                \
    {0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86} }

#define EFI_EDID_DISCOVERED_GUID                                               \
  { 0x1c0c34f6, 0xd380, 0x41fa,                                                \
    {0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa} }

#define EFI_ACPI_20_TABLE_GUID                                                 \
  { 0x8868e871, 0xe4f1, 0x11d3,                                                \
    {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81} }

#define EFI_ACPI_10_TABLE_GUID                                                 \
  { 0xeb9d2d30, 0x2d88, 0x11d3,                                                \
    {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d} }

#define EFI_PIXEL_RGBX 0
#define EFI_PIXEL_BGRX 1
#define EFI_PIXEL_BITMASK 2
#define EFI_PIXEL_BLT_ONLY 3

struct efi_pixel_bitmask {
  uint32 red;
  uint32 green;
  uint32 blue;
  uint32 reserved;
};

struct efi_gop_mode_info {
  uint32 version;
  uint32 width;
  uint32 height;
  uint32 pixel_format;
  struct efi_pixel_bitmask pixel_information;
  uint32 pixels_per_scanline;
};

struct efi_gop_mode {
  uint32 max_mode;
  uint32 mode;
  struct efi_gop_mode_info *info;
  efi_uintn info_size;
  efi_phys framebuffer_base;
  efi_uintn framebuffer_size;
};

struct efi_graphics_output {
  efi_status(EFIAPI *query_mode)(struct efi_graphics_output *, uint32 mode,
                                 efi_uintn *size,
                                 struct efi_gop_mode_info **info);
  efi_status(EFIAPI *set_mode)(struct efi_graphics_output *, uint32 mode);
  void *blt;
  struct efi_gop_mode *mode;
};

struct efi_edid {
  uint32 size;
  uint8 *edid;
};

#endif
