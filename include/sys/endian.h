#ifndef _SYS_ENDIAN_H_
#define _SYS_ENDIAN_H_

#include <sys/types.h>

#define swap16(x) ((uint16_t)(((x) >> 8) | ((x) << 8)))
#define swap32(x)                                                              \
  ((uint32_t)((((x) & 0xffu) << 24) | (((x) & 0xff00u) << 8) |                 \
              (((x) >> 8) & 0xff00u) | (((x) >> 24) & 0xffu)))
#define swap64(x)                                                              \
  ((uint64_t)(((uint64_t)swap32((uint32_t)(x)) << 32) |                        \
              swap32((uint32_t)((x) >> 32))))

#define htobe16(x) swap16(x)
#define htobe32(x) swap32(x)
#define htobe64(x) swap64(x)
#define be16toh(x) swap16(x)
#define be32toh(x) swap32(x)
#define be64toh(x) swap64(x)
#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define le64toh(x) ((uint64_t)(x))
#define betoh16(x) be16toh(x)
#define betoh32(x) be32toh(x)
#define betoh64(x) be64toh(x)
#define letoh16(x) le16toh(x)
#define letoh32(x) le32toh(x)
#define letoh64(x) le64toh(x)

#ifndef htons
#define htons(x) htobe16(x)
#define htonl(x) htobe32(x)
#define ntohs(x) htobe16(x)
#define ntohl(x) htobe32(x)
#endif

#define NTOHL(x) (x) = ntohl((u_int32_t)(x))
#define NTOHS(x) (x) = ntohs((u_int16_t)(x))
#define HTONL(x) (x) = htonl((u_int32_t)(x))
#define HTONS(x) (x) = htons((u_int16_t)(x))

#endif
