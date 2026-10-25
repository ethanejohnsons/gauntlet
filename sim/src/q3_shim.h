// q3_shim.h -- hooks into q3_shim.c used by the loader

#ifndef Q3_SHIM_H
#define Q3_SHIM_H

#include <setjmp.h>

extern int gs_verbose;

void GS_SetErrorJump( jmp_buf *jb );
const char *GS_LastError( void );
void GS_SetPendingFile( void *data, int len );
void GS_HunkClear( void );
void GS_BumpMapGeneration( void );

#endif
