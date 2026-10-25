// q3_shim.c -- the few engine services the vendored collision and movement code
// needs: memory, file reads, cvars, printing, errors and SnapVector. Plus the
// per-thread visit stamps used by the patched cm_trace.c / cm_test.c.

#include <setjmp.h>
#include <pthread.h>
#include <limits.h>

#include "cm_local.h"
#include "q3_shim.h"

// ---------------------------------------------------------------------------
// errors and printing

static _Thread_local jmp_buf *gs_errorJmp;
static _Thread_local char gs_errorMsg[1024];
int gs_verbose = 0;

void GS_SetErrorJump( jmp_buf *jb ) {
	gs_errorJmp = jb;
}

const char *GS_LastError( void ) {
	return gs_errorMsg;
}

void QDECL Com_Error( errorParm_t level, const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( gs_errorMsg, sizeof( gs_errorMsg ), fmt, ap );
	va_end( ap );
	if ( gs_errorJmp ) {
		longjmp( *gs_errorJmp, 1 );
	}
	fprintf( stderr, "gauntlet: fatal: %s\n", gs_errorMsg );
	abort();
}

void QDECL Com_Printf( const char *fmt, ... ) {
	va_list ap;
	if ( !gs_verbose ) {
		return;
	}
	va_start( ap, fmt );
	vfprintf( stderr, fmt, ap );
	va_end( ap );
}

void QDECL Com_DPrintf( const char *fmt, ... ) {
	va_list ap;
	if ( gs_verbose < 2 ) {
		return;
	}
	va_start( ap, fmt );
	vfprintf( stderr, fmt, ap );
	va_end( ap );
}

// ---------------------------------------------------------------------------
// memory: the hunk lives as long as the loaded map, so it's a plain list of blocks

typedef struct hunkBlock_s {
	struct hunkBlock_s *next;
} hunkBlock_t;

static hunkBlock_t *gs_hunk;

void *Hunk_Alloc( size_t size, ha_pref preference ) {
	hunkBlock_t *b = calloc( 1, sizeof( hunkBlock_t ) + size + 32 );
	if ( !b ) {
		Com_Error( ERR_FATAL, "Hunk_Alloc: out of memory (%zu bytes)", size );
	}
	b->next = gs_hunk;
	gs_hunk = b;
	// keep the 32-byte alignment the engine hunk gives
	return (void *)( ( (uintptr_t)( b + 1 ) + 31 ) & ~(uintptr_t)31 );
}

void GS_HunkClear( void ) {
	while ( gs_hunk ) {
		hunkBlock_t *next = gs_hunk->next;
		free( gs_hunk );
		gs_hunk = next;
	}
}

void *Hunk_AllocateTempMemory( size_t size ) {
	void *p = malloc( size );
	if ( !p ) {
		Com_Error( ERR_FATAL, "Hunk_AllocateTempMemory: out of memory" );
	}
	return p;
}

void Hunk_FreeTempMemory( void *buf ) {
	free( buf );
}

void *Z_Malloc( size_t size ) {
	void *p = calloc( 1, size );
	if ( !p ) {
		Com_Error( ERR_FATAL, "Z_Malloc: out of memory" );
	}
	return p;
}

void Z_Free( void *ptr ) {
	free( ptr );
}

// ---------------------------------------------------------------------------
// files: CM_LoadMap reads the BSP through FS_ReadFile; the loader hands it over here

static void *gs_pendingFile;
static int gs_pendingLen;

void GS_SetPendingFile( void *data, int len ) {
	gs_pendingFile = data;
	gs_pendingLen = len;
}

int FS_ReadFile( const char *qpath, void **buffer ) {
	if ( !gs_pendingFile ) {
		if ( buffer ) {
			*buffer = NULL;
		}
		return -1;
	}
	if ( buffer ) {
		*buffer = gs_pendingFile;
		gs_pendingFile = NULL;
	}
	return gs_pendingLen;
}

void FS_FreeFile( void *buffer ) {
	free( buffer );
}

unsigned Com_BlockChecksum( const void *buffer, int length ) {
	// FNV-1a. The engine uses MD4 here, but the checksum is only reported, never compared.
	const byte *p = buffer;
	unsigned h = 2166136261u;
	for ( int i = 0; i < length; i++ ) {
		h = ( h ^ p[i] ) * 16777619u;
	}
	return h;
}

// ---------------------------------------------------------------------------
// cvars: collision reads a handful at load time; all keep their engine defaults

#define GS_MAX_CVARS 32
static cvar_t gs_cvars[GS_MAX_CVARS];
static int gs_numCvars;

cvar_t *Cvar_Get( const char *var_name, const char *value, int flags ) {
	for ( int i = 0; i < gs_numCvars; i++ ) {
		if ( !strcmp( gs_cvars[i].name, var_name ) ) {
			return &gs_cvars[i];
		}
	}
	if ( gs_numCvars == GS_MAX_CVARS ) {
		Com_Error( ERR_FATAL, "Cvar_Get: too many cvars" );
	}
	cvar_t *cv = &gs_cvars[gs_numCvars++];
	cv->name = strdup( var_name );
	cv->string = strdup( value );
	cv->resetString = cv->string;
	cv->flags = flags;
	cv->value = Q_atof( value );
	cv->integer = atoi( value );
	return cv;
}

void Cvar_SetDescription( cvar_t *var, const char *var_description ) {
}

void BotDrawDebugPolygons( void (*drawPoly)(int color, int numPoints, float *points), int value ) {
}

// ---------------------------------------------------------------------------
// G_SNAPVECTOR as oDFe implements it on x86_64 Linux (Sys_SnapVector in common.c)

void trap_SnapVector( float *v ) {
	v[0] = rint( v[0] );
	v[1] = rint( v[1] );
	v[2] = rint( v[2] );
}

// ---------------------------------------------------------------------------
// per-thread brush/patch visit stamps (see the GAUNTLET_SIM block in cm_local.h)

_Thread_local int gs_checkcount;
_Thread_local int *gs_brushStamp;
_Thread_local int *gs_patchStamp;
static _Thread_local int gs_stampGeneration;
static int gs_mapGeneration;	// bumped on every map load

void GS_BumpMapGeneration( void ) {
	gs_mapGeneration++;
}

void GS_EnsureStamps( void ) {
	if ( gs_stampGeneration != gs_mapGeneration ) {
		free( gs_brushStamp );
		free( gs_patchStamp );
		gs_brushStamp = calloc( cm.numBrushes + 1, sizeof( int ) );
		gs_patchStamp = calloc( cm.numSurfaces + 1, sizeof( int ) );
		gs_checkcount = 0;
		gs_stampGeneration = gs_mapGeneration;
	}
	if ( gs_checkcount >= INT_MAX - 16 ) {
		// wrap safely: a stale stamp equal to the counter would skip a brush
		memset( gs_brushStamp, 0, ( cm.numBrushes + 1 ) * sizeof( int ) );
		memset( gs_patchStamp, 0, ( cm.numSurfaces + 1 ) * sizeof( int ) );
		gs_checkcount = 0;
	}
}
