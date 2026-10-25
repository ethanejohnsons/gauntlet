// test_observe -- observations must be finite, in a sensible range, and the same
// whatever the thread count

#include "common.h"

#define NUM_ENVS	256

int main( int argc, char **argv ) {
	const char *map = argc > 1 ? argv[1] : "vanilla_01";
	static gs_env_t *envs[NUM_ENVS];
	static gs_cmd_t cmds[NUM_ENVS];
	static float a[NUM_ENVS * GS_OBS_SIZE], b[NUM_ENVS * GS_OBS_SIZE];
	static float routeA[NUM_ENVS], routeB[NUM_ENVS];

	load_or_die( map );
	CHECK( gs_route_build( 0 ) == 0, "route build failed: %s", gs_last_error() );
	srand( 7 );
	for ( int i = 0; i < NUM_ENVS; i++ ) {
		envs[i] = gs_env_new();
		gs_env_spawn( envs[i] );
	}
	// wander a bit so the players are spread out
	for ( int step = 0; step < 200; step++ ) {
		for ( int i = 0; i < NUM_ENVS; i++ ) {
			cmds[i] = ( gs_cmd_t ){ { 0, rand() & 65535, 0 }, 127, rand() % 3 * 127 - 127, rand() % 5 ? 0 : 127, 0 };
		}
		gs_step_batch( envs, cmds, NUM_ENVS, 8 );
	}

	gs_observe_batch( envs, NUM_ENVS, 1, a, NULL, routeA );
	gs_observe_batch( envs, NUM_ENVS, 8, b, NULL, routeB );
	CHECK( !memcmp( a, b, sizeof( a ) ) && !memcmp( routeA, routeB, sizeof( routeA ) ),
		   "observations differ between 1 and 8 threads" );

	float lo = 1e9f, hi = -1e9f;
	for ( int i = 0; i < NUM_ENVS * GS_OBS_SIZE; i++ ) {
		CHECK( isfinite( a[i] ), "observation %d of env %d is not finite", i % GS_OBS_SIZE, i / GS_OBS_SIZE );
		lo = a[i] < lo ? a[i] : lo;
		hi = a[i] > hi ? a[i] : hi;
	}
	// feelers are fractions; the rest is scaled to stay near [-1, 1]
	for ( int i = 0; i < NUM_ENVS; i++ ) {
		for ( int k = 7; k <= 32; k++ ) {
			float f = a[i * GS_OBS_SIZE + k];
			CHECK( f >= 0 && f <= 1, "feeler %d of env %d is %f", k, i, f );
		}
	}
	printf( "%d envs: observations in [%.2f, %.2f], identical on 1 and 8 threads\n", NUM_ENVS, lo, hi );
	CHECK( lo > -4 && hi < 4, "observations should stay roughly within [-1, 1]" );
	printf( "ok\n" );
	return 0;
}
