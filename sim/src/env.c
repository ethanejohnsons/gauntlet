// env.c -- the players: creating, stepping, placing and saving envs, and the thread
// pool that steps (gs_step_batch) or observes (gs_observe_batch) many at once.

#include <pthread.h>
#include <stdatomic.h>

#include "sim_local.h"

gs_env_t *gs_env_new( void ) {
	return calloc( 1, sizeof( gs_env_t ) );
}

void gs_env_free( gs_env_t *env ) {
	free( env );
}

void gs_env_spawn( gs_env_t *env ) {
	G_SpawnPlayer( env );
}

void gs_env_set_route_target( gs_env_t *env, int target ) {
	env->routeTarget = target;
}

void gs_env_step( gs_env_t *env, const gs_cmd_t *in ) {
	usercmd_t cmd;
	memset( &cmd, 0, sizeof( cmd ) );
	// the client sends one command per 8 ms frame (com_maxfps 125)
	cmd.serverTime = env->ps.commandTime + GS_FRAME_MSEC;
	for ( int i = 0; i < 3; i++ ) {
		cmd.angles[i] = in->angles[i];
	}
	cmd.forwardmove = in->forward;
	cmd.rightmove = in->right;
	cmd.upmove = in->up;
	cmd.buttons = in->buttons;
	cmd.weapon = in->weapon ? in->weapon : env->ps.weapon;
	G_ClientThink( env, &cmd );
	env->frame++;
}

void gs_env_place( gs_env_t *env, const float origin[3], const float velocity[3], float yaw ) {
	G_PlacePlayer( env, origin, velocity, yaw );
}

void gs_env_set_ammo( gs_env_t *env, int weapon, int ammo ) {
	if ( weapon > WP_NONE && weapon < WP_NUM_WEAPONS ) {
		env->ps.ammo[weapon] = ammo;
	}
}

void gs_env_get_state( const gs_env_t *env, gs_state_t *out ) {
	const playerState_t *ps = &env->ps;
	VectorCopy( ps->origin, out->origin );
	VectorCopy( ps->velocity, out->velocity );
	VectorCopy( ps->viewangles, out->viewangles );
	for ( int i = 0; i < 3; i++ ) {
		out->delta_angles[i] = ps->delta_angles[i];
	}
	out->command_time = ps->commandTime;
	out->pm_flags = ps->pm_flags;
	out->pm_time = ps->pm_time;
	out->ground_entity = ps->groundEntityNum == ENTITYNUM_NONE ? -1 : ps->groundEntityNum;
	out->frame = env->frame;
	out->timer_state = env->timerState;
	out->timer_ms = env->timerMs;
	out->checkpoints = env->checkpoints;
	out->dead = env->dead;
	out->haste_ms = ps->powerups[PW_HASTE] ? ps->powerups[PW_HASTE] - env->levelTime : 0;
	out->flight_ms = ps->powerups[PW_FLIGHT] ? ps->powerups[PW_FLIGHT] - env->levelTime : 0;
	out->health = ps->stats[STAT_HEALTH];
	out->armor = ps->stats[STAT_ARMOR];
	out->weapon = ps->weapon;
	out->weapons = ps->stats[STAT_WEAPONS];
	out->rockets = ps->ammo[WP_ROCKET_LAUNCHER];
	out->weapon_time = ps->weaponTime;
	out->rockets_in_flight = 0;
	for ( int i = 0; i < GS_MAX_MISSILES; i++ ) {
		out->rockets_in_flight += env->missiles[i].active;
	}
}

size_t gs_env_snapshot_size( void ) {
	return sizeof( gs_env_t );
}

void gs_env_save( const gs_env_t *env, void *buf ) {
	memcpy( buf, env, sizeof( *env ) );
}

void gs_env_restore( gs_env_t *env, const void *buf ) {
	memcpy( env, buf, sizeof( *env ) );
}

// ---------------------------------------------------------------------------
// GS_ParallelFor: run fn( i, ctx ) for every i < n on a pool of threads. Helper
// threads are started on first use and then wait for work. The range is cut into
// chunks of GS_CHUNK; every thread, including the caller, keeps taking the next
// chunk until none are left.

#define GS_MAX_THREADS	256
#define GS_CHUNK		16

static struct {
	pthread_mutex_t	lock;
	pthread_cond_t	start;			// signalled when a new job is ready
	pthread_cond_t	done;			// signalled when the last helper finishes
	pthread_t		threads[GS_MAX_THREADS];
	int				numThreads;
	int				job;			// incremented for every job
	int				busy;			// helpers still working on the current job

	void			( *fn )( int i, void *ctx );
	void			*ctx;
	int				n;
	atomic_int		next;			// first index of the next chunk
} pool = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void RunChunks( void ) {
	while ( 1 ) {
		int i = atomic_fetch_add( &pool.next, GS_CHUNK );
		if ( i >= pool.n ) {
			return;
		}
		int end = i + GS_CHUNK < pool.n ? i + GS_CHUNK : pool.n;
		for ( ; i < end; i++ ) {
			pool.fn( i, pool.ctx );
		}
	}
}

static void *Helper( void *arg ) {
	int lastJob = 0;
	pthread_mutex_lock( &pool.lock );
	while ( 1 ) {
		while ( pool.job == lastJob ) {
			pthread_cond_wait( &pool.start, &pool.lock );
		}
		lastJob = pool.job;
		pthread_mutex_unlock( &pool.lock );

		RunChunks();

		pthread_mutex_lock( &pool.lock );
		if ( --pool.busy == 0 ) {
			pthread_cond_signal( &pool.done );
		}
	}
	return NULL;
}

void GS_ParallelFor( int n, int nthreads, void ( *fn )( int i, void *ctx ), void *ctx ) {
	if ( nthreads <= 1 || n <= GS_CHUNK ) {
		for ( int i = 0; i < n; i++ ) {
			fn( i, ctx );
		}
		return;
	}
	if ( nthreads > GS_MAX_THREADS ) {
		nthreads = GS_MAX_THREADS;
	}

	pthread_mutex_lock( &pool.lock );
	while ( pool.numThreads < nthreads - 1 ) {		// the caller is the last thread
		pthread_create( &pool.threads[pool.numThreads++], NULL, Helper, NULL );
	}
	pool.fn = fn;
	pool.ctx = ctx;
	pool.n = n;
	atomic_store( &pool.next, 0 );
	pool.busy = pool.numThreads;
	pool.job++;
	pthread_cond_broadcast( &pool.start );
	pthread_mutex_unlock( &pool.lock );

	RunChunks();

	pthread_mutex_lock( &pool.lock );
	while ( pool.busy > 0 ) {
		pthread_cond_wait( &pool.done, &pool.lock );
	}
	pthread_mutex_unlock( &pool.lock );
}

typedef struct {
	gs_env_t		**envs;
	const gs_cmd_t	*cmds;
} stepJob_t;

static void StepOne( int i, void *ctx ) {
	stepJob_t *job = ctx;
	gs_env_step( job->envs[i], &job->cmds[i] );
}

void gs_step_batch( gs_env_t **envs, const gs_cmd_t *cmds, int n, int nthreads ) {
	stepJob_t job = { envs, cmds };
	GS_ParallelFor( n, nthreads, StepOne, &job );
}
