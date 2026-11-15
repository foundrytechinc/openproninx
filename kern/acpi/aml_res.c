// AML RESOURCES

#include "acpi.h"
#include "aml.h"
#include "aml_priv.h"
#include "defs.h"

static union acpi_resource *mapresource(union acpi_resource *crs) {
  static union acpi_resource map;
  int rlen = AML_CRSLEN(crs);

  if (rlen >= (int)sizeof(map))
    return crs;
  memset(&map, 0, sizeof(map));
  memcpy(&map, crs, (size_t)rlen);
  return &map;
}

int aml_parse_resource(struct aml_value *res,
                       int (*cb)(int, union acpi_resource *, void *),
                       void *arg) {
  union acpi_resource *crs;
  int off, rlen, idx;

  if (res->type != AML_BUFFER || res->length < 5)
    return -1;
  for (off = 0, idx = 0; off < res->length; off += rlen, idx++) {
    crs = (union acpi_resource *)(res->buf + off);
    rlen = AML_CRSLEN(crs);
    if (crs->hdr.typecode == SRT_ENDTAG || rlen == 0)
      break;
    if (off + rlen > res->length)
      break;
    cb(idx, mapresource(crs), arg);
  }
  return 0;
}
