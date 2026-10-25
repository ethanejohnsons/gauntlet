// bench -- steps per second for random keyboard-style input, across thread counts

#include <time.h>
#include <unistd.h>

#include "common.h"

#define NUM_ENVS	4096
#define NUM_STEPS	250

static double now( void ) {
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main( int argc, char **argv ) {
	const char *map = argc > 1 ? argv[1] : "vanilla_01";
	static gs_env_t *envs[NUM_ENVS];
	static gs_cmd_t cmds[NUM_STEPS][NUM_ENVS];
	static const int8_t keys[3] = { -127, 0, 127 };
	int maxThreads = (int)sysconf( _SC_NPROCESSORS_ONLN );

	load_or_die( map );
	srand( 1 );
	for ( int t = 0; t < NUM_STEPS; t++ ) {
		for ( int i = 0; i < NUM_ENVS; i++ ) {
			gs_cmd_t *c = &cmds[t][i];
			memset( c, 0, sizeof( *c ) );
			c->forward = keys[rand() % 3];
			c->right = keys[rand() % 3];
			c->up = rand() % 4 == 0 ? 127 : 0;
			c->angles[1] = rand() & 65535;
		}
	}
	for ( int i = 0; i < NUM_ENVS; i++ ) {
		envs[i] = gs_env_new();
	}

	// 1, 2, 4, ... threads, ending with every core
	int counts[16], numCounts = 0;
	for ( int t = 1; t < maxThreads && numCounts < 15; t *= 2 ) {
		counts[numCounts++] = t;
	}
	counts[numCounts++] = maxThreads;

	printf( "%s: %d envs x %d steps\n", map, NUM_ENVS, NUM_STEPS );
	for ( int c = 0; c < numCounts; c++ ) {
		int threads = counts[c];
		for ( int i = 0; i < NUM_ENVS; i++ ) {
			gs_env_spawn( envs[i] );
		}
		double t0 = now();
		for ( int t = 0; t < NUM_STEPS; t++ ) {
			gs_step_batch( envs, cmds[t], NUM_ENVS, threads );
		}
		double dt = now() - t0;
		double sps = (double)NUM_ENVS * NUM_STEPS / dt;
		printf( "  %2d threads: %10.0f steps/s = %7.0f seconds of play per second\n",
				threads, sps, sps * GS_FRAME_MSEC / 1000.0 );
	}
	return 0;
}
