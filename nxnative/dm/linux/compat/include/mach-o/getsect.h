// Android port: the host's registration tables are ELF sections named after the Mach-O section (hle.h DM_SECTION), bounded by the linker's
// __start_<name> / __stop_<name> symbols; getsectiondata looks the section name up among those (the segment name is ignored). Any other section: NULL.
#pragma once
#include <stdint.h>
struct mach_header_64 { uint32_t magic; };
extern const struct mach_header_64 _mh_execute_header;
uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size);
