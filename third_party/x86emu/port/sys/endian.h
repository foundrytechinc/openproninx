// x86emu's sys/endian.h: amd64 is little-endian
#ifndef X86EMU_PORT_ENDIAN_H
#define X86EMU_PORT_ENDIAN_H

#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define letoh16(x) ((uint16_t)(x))
#define letoh32(x) ((uint32_t)(x))

#endif
