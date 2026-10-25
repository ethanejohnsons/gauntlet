// common.h -- shared helpers for the libgauntlet tests

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gauntlet.h"

#define CHECK( cond, ... ) do { if ( !( cond ) ) { \
	fprintf( stderr, "FAIL %s:%d: ", __FILE__, __LINE__ ); fprintf( stderr, __VA_ARGS__ ); \
	fprintf( stderr, "\n" ); exit( 1 ); } } while ( 0 )

// game data: $GAUNTLET_Q3, or ~/Games/quake3 (see docs/setup.md)
static const char *q3_path( void ) {
	static char path[1024];
	const char *p = getenv( "GAUNTLET_Q3" );
	if ( p && p[0] ) {
		return p;
	}
	snprintf( path, sizeof( path ), "%s/Games/quake3", getenv( "HOME" ) ? getenv( "HOME" ) : "." );
	return path;
}

static void load_or_die( const char *map ) {
	if ( gs_load_map( q3_path(), "defrag", map ) < 0 ) {
		fprintf( stderr, "FAIL loading %s: %s\n", map, gs_last_error() );
		exit( 1 );
	}
}

// raw cmd angle that makes the view look at `deg`, given the env's delta_angles
static int32_t cmd_angle( float deg, int32_t delta ) {
	return ( ( (int32_t)( deg * 65536 / 360 ) & 65535 ) - delta ) & 65535;
}

static float hspeed( const gs_state_t *s ) {
	return sqrtf( s->velocity[0] * s->velocity[0] + s->velocity[1] * s->velocity[1] );
}
