// bg_vmcompat.h -- force-included (-include) into the game-side sources: bg_*.c and
// q_math_vm.c. Not part of the original Quake III source.
//
// DeFRaG's movement runs as a QVM inside oDFe. The QVM computes like this:
//  - "double" is 32-bit, so every float expression and constant is single precision.
//    Native builds get the same with -fsingle-precision-constant.
//  - sin/cos/atan2/sqrt are syscalls: float in, the engine's double libm call,
//    float out. The wrappers below give native code the same rounding.
//  - The math helpers come from id's game-side q_math.c, not the engine's.
//    They are renamed so both versions can live in one library.

#ifndef BG_VMCOMPAT_H
#define BG_VMCOMPAT_H

#include <math.h>

static inline float vm_sin( float x ) { return (float)sin( x ); }
static inline float vm_cos( float x ) { return (float)cos( x ); }
static inline float vm_atan2( float y, float x ) { return (float)atan2( y, x ); }
static inline float vm_sqrt( float x ) { return (float)sqrt( x ); }

#define sin( x )		vm_sin( x )
#define cos( x )		vm_cos( x )
#define atan2( y, x )	vm_atan2( y, x )
#define sqrt( x )		vm_sqrt( x )

#define AngleNormalize360	BGM_AngleNormalize360
#define AngleNormalize180	BGM_AngleNormalize180
#define vectoangles			BGM_vectoangles
#define VectorNormalize		BGM_VectorNormalize
#define VectorNormalize2	BGM_VectorNormalize2
#define AngleVectors		BGM_AngleVectors

// engine syscall (G_SNAPVECTOR), implemented in src/q3_shim.c
void trap_SnapVector( float *v );

#endif
