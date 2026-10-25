// test_threads -- the same random inputs must give bit-identical results whether
// envs are stepped one by one or spread across threads

#include "common.h"

#define NUM_ENVS	512
#define NUM_STEPS	500

static uint32_t rng = 12345;
static uint32_t next_rand( void ) {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static gs_cmd_t random_cmd( void ) {
	static const int8_t keys[3] = { -127, 0, 127 };
	gs_cmd_t cmd = { 0 };
	uint32_t r = next_rand();
	cmd.forward = keys[r % 3];
	cmd.right = keys[( r >> 2 ) % 3];
	cmd.up = ( r >> 4 ) % 4 == 0 ? 127 : 0;
	cmd.angles[1] = next_rand() & 65535;
	cmd.angles[0] = ( next_rand() % 4000 ) - 2000;
	return cmd;
}

int main( int argc, char **argv ) {
	const char *map = argc > 1 ? argv[1] : "vanilla_01";
	static gs_env_t *a[NUM_ENVS], *b[NUM_ENVS];
	static gs_cmd_t cmds[NUM_ENVS];

	load_or_die( map );
	for ( int i = 0; i < NUM_ENVS; i++ ) {
		a[i] = gs_env_new();
		b[i] = gs_env_new();
		gs_env_spawn( a[i] );
		gs_env_spawn( b[i] );
	}
	for ( int step = 0; step < NUM_STEPS; step++ ) {
		for ( int i = 0; i < NUM_ENVS; i++ ) {
			cmds[i] = random_cmd();
		}
		gs_step_batch( a, cmds, NUM_ENVS, 1 );
		gs_step_batch( b, cmds, NUM_ENVS, 8 );
	}

	size_t size = gs_env_snapshot_size();
	unsigned char *sa = malloc( size ), *sb = malloc( size );
	int moved = 0;
	for ( int i = 0; i < NUM_ENVS; i++ ) {
		gs_env_save( a[i], sa );
		gs_env_save( b[i], sb );
		CHECK( !memcmp( sa, sb, size ), "env %d differs between 1 and 8 threads", i );
		gs_state_t s;
		gs_env_get_state( a[i], &s );
		moved += hspeed( &s ) > 1 || s.ground_entity < 0;
	}
	printf( "%d envs x %d steps identical on 1 and 8 threads (%d still moving)\n", NUM_ENVS, NUM_STEPS, moved );
	printf( "ok\n" );
	return 0;
}
