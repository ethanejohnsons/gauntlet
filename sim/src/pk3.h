#ifndef PK3_H
#define PK3_H

#include <stddef.h>

// Read `name` (e.g. "maps/vanilla_01.bsp") from <basepath>/<game>/*.pk3, falling back
// to <basepath>/baseq3/*.pk3. Returns 1 and a malloc'd buffer on success.
// If srcpk3 is given, it receives the path of the archive the file came from.
int pk3_read_file( const char *basepath, const char *game, const char *name,
				   void **out, int *outlen, char *srcpk3, size_t srclen );

#endif
