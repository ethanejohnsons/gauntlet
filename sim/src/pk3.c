// pk3.c -- read a file out of the pk3 (zip) archives in a Quake III install.
// Same priority as the engine: the mod folder beats baseq3, and inside a folder the
// archive that sorts last wins.

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "sim_local.h"

static uint16_t Read16( const uint8_t *p ) { return p[0] | p[1] << 8; }
static uint32_t Read32( const uint8_t *p ) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Look for `name` (case-insensitive) in one zip. Returns 1 and the file's data if found.
static int ZipRead( const char *zippath, const char *name, void **out, int *outlen ) {
	FILE *f = fopen( zippath, "rb" );
	if ( !f ) {
		return 0;
	}
	int found = 0;
	uint8_t *cd = NULL, *comp = NULL, *data = NULL;

	// end of central directory record: within the last 64KB + 22 bytes
	fseek( f, 0, SEEK_END );
	long size = ftell( f );
	long tail = size < 65557 ? size : 65557;
	uint8_t *buf = malloc( tail );
	fseek( f, size - tail, SEEK_SET );
	if ( fread( buf, 1, tail, f ) != (size_t)tail ) {
		goto done;
	}
	long eocd = -1;
	for ( long i = tail - 22; i >= 0; i-- ) {
		if ( Read32( buf + i ) == 0x06054b50 ) {
			eocd = i;
			break;
		}
	}
	if ( eocd < 0 ) {
		goto done;
	}
	uint32_t cdCount = Read16( buf + eocd + 10 );
	uint32_t cdSize = Read32( buf + eocd + 12 );
	uint32_t cdOfs = Read32( buf + eocd + 16 );

	cd = malloc( cdSize );
	fseek( f, cdOfs, SEEK_SET );
	if ( fread( cd, 1, cdSize, f ) != cdSize ) {
		goto done;
	}

	size_t namelen = strlen( name );
	uint8_t *p = cd;
	for ( uint32_t i = 0; i < cdCount && p + 46 <= cd + cdSize; i++ ) {
		if ( Read32( p ) != 0x02014b50 ) {
			break;
		}
		uint16_t method = Read16( p + 10 );
		uint32_t csize = Read32( p + 20 ), usize = Read32( p + 24 );
		uint16_t fnlen = Read16( p + 28 ), exlen = Read16( p + 30 ), cmlen = Read16( p + 32 );
		uint32_t lhOfs = Read32( p + 42 );
		const char *fn = (const char *)p + 46;

		if ( fnlen == namelen && !strncasecmp( fn, name, namelen ) && ( method == 0 || method == 8 ) ) {
			uint8_t lh[30];
			fseek( f, lhOfs, SEEK_SET );
			if ( fread( lh, 1, 30, f ) != 30 || Read32( lh ) != 0x04034b50 ) {
				goto done;
			}
			fseek( f, lhOfs + 30 + Read16( lh + 26 ) + Read16( lh + 28 ), SEEK_SET );
			comp = malloc( csize ? csize : 1 );
			if ( fread( comp, 1, csize, f ) != csize ) {
				goto done;
			}
			data = malloc( usize ? usize : 1 );
			if ( method == 0 ) {
				memcpy( data, comp, usize );
			} else {
				z_stream zs = { 0 };
				zs.next_in = comp;
				zs.avail_in = csize;
				zs.next_out = data;
				zs.avail_out = usize;
				if ( inflateInit2( &zs, -MAX_WBITS ) != Z_OK ) {
					goto done;
				}
				int r = inflate( &zs, Z_FINISH );
				inflateEnd( &zs );
				if ( r != Z_STREAM_END || zs.total_out != usize ) {
					goto done;
				}
			}
			*out = data;
			*outlen = (int)usize;
			data = NULL;
			found = 1;
			goto done;
		}
		p += 46 + fnlen + exlen + cmlen;
	}

done:
	free( buf );
	free( cd );
	free( comp );
	free( data );
	fclose( f );
	return found;
}

static int Pk3Compare( const struct dirent **a, const struct dirent **b ) {
	// the engine orders pk3s with a case-insensitive compare (FS_PathCmp)
	return strcasecmp( ( *a )->d_name, ( *b )->d_name );
}

static int IsPk3( const struct dirent *d ) {
	size_t n = strlen( d->d_name );
	return n > 4 && !strcasecmp( d->d_name + n - 4, ".pk3" );
}

// Search one directory; the last (highest-sorting) pk3 containing the file wins.
static int DirRead( const char *dir, const char *name, void **out, int *outlen, char *srcpk3, size_t srclen ) {
	struct dirent **list;
	int n = scandir( dir, &list, IsPk3, Pk3Compare );
	if ( n < 0 ) {
		return 0;
	}
	int found = 0;
	for ( int i = n - 1; i >= 0 && !found; i-- ) {
		char path[4096];
		snprintf( path, sizeof( path ), "%s/%s", dir, list[i]->d_name );
		if ( ZipRead( path, name, out, outlen ) ) {
			found = 1;
			if ( srcpk3 ) {
				snprintf( srcpk3, srclen, "%s", path );
			}
		}
	}
	for ( int i = 0; i < n; i++ ) {
		free( list[i] );
	}
	free( list );
	return found;
}

int GS_ReadPk3File( const char *basepath, const char *game, const char *name,
					void **out, int *outlen, char *srcpk3, size_t srclen ) {
	char dir[4096];
	// the mod directory overrides baseq3
	if ( game && game[0] ) {
		snprintf( dir, sizeof( dir ), "%s/%s", basepath, game );
		if ( DirRead( dir, name, out, outlen, srcpk3, srclen ) ) {
			return 1;
		}
	}
	snprintf( dir, sizeof( dir ), "%s/baseq3", basepath );
	return DirRead( dir, name, out, outlen, srcpk3, srclen );
}
