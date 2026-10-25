// pool.c -- gs_step_batch: a persistent worker pool. Workers take chunks of envs
// off a shared counter, and the calling thread works too.

#include <pthread.h>
#include <stdatomic.h>

#include "sim_local.h"

#define GS_MAX_THREADS	256
#define GS_CHUNK		16

static struct {
	pthread_mutex_t	lock;
	pthread_cond_t	start, done;
	pthread_t		threads[GS_MAX_THREADS];
	int				numThreads;
	int				generation;
	int				busy;

	gs_env_t		**envs;
	const gs_cmd_t	*cmds;
	int				n;
	atomic_int		next;
} pool = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void RunChunks( void ) {
	while ( 1 ) {
		int i = atomic_fetch_add( &pool.next, GS_CHUNK );
		if ( i >= pool.n ) {
			return;
		}
		int end = i + GS_CHUNK < pool.n ? i + GS_CHUNK : pool.n;
		for ( ; i < end; i++ ) {
			gs_env_step( pool.envs[i], &pool.cmds[i] );
		}
	}
}

static void *Worker( void *arg ) {
	int seen = 0;
	pthread_mutex_lock( &pool.lock );
	while ( 1 ) {
		while ( pool.generation == seen ) {
			pthread_cond_wait( &pool.start, &pool.lock );
		}
		seen = pool.generation;
		pthread_mutex_unlock( &pool.lock );

		RunChunks();

		pthread_mutex_lock( &pool.lock );
		if ( --pool.busy == 0 ) {
			pthread_cond_signal( &pool.done );
		}
	}
	return NULL;
}

void gs_step_batch( gs_env_t **envs, const gs_cmd_t *cmds, int n, int nthreads ) {
	if ( nthreads <= 1 || n <= GS_CHUNK ) {
		for ( int i = 0; i < n; i++ ) {
			gs_env_step( envs[i], &cmds[i] );
		}
		return;
	}
	if ( nthreads > GS_MAX_THREADS ) {
		nthreads = GS_MAX_THREADS;
	}

	pthread_mutex_lock( &pool.lock );
	// the calling thread works too, so it needs nthreads - 1 helpers
	while ( pool.numThreads < nthreads - 1 ) {
		pthread_create( &pool.threads[pool.numThreads++], NULL, Worker, NULL );
	}
	pool.envs = envs;
	pool.cmds = cmds;
	pool.n = n;
	atomic_store( &pool.next, 0 );
	pool.busy = pool.numThreads;
	pool.generation++;
	pthread_cond_broadcast( &pool.start );
	pthread_mutex_unlock( &pool.lock );

	RunChunks();

	pthread_mutex_lock( &pool.lock );
	while ( pool.busy > 0 ) {
		pthread_cond_wait( &pool.done, &pool.lock );
	}
	pthread_mutex_unlock( &pool.lock );
}
