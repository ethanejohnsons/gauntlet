// observe.c -- what a player senses, for training: its own motion, feelers (rays)
// around it, the route ahead, the timer, and its weapon and health. The layout is
// documented in gauntlet.h.

#include "sim_local.h"

static const float routeAhead[4] = { 64, 128, 256, 512 };

// x forward, y left, z up, from the view yaw
static void YawAxes( float yawDegrees, vec3_t forward, vec3_t left ) {
	float yaw = DEG2RAD( yawDegrees );
	VectorSet( forward, cosf( yaw ), sinf( yaw ), 0 );
	VectorSet( left, -sinf( yaw ), cosf( yaw ), 0 );
}

// a world-space vector in the player's frame, scaled
static void ToPlayerFrame( const vec3_t v, const vec3_t forward, const vec3_t left, float scale, float *out ) {
	out[0] = DotProduct( v, forward ) / scale;
	out[1] = DotProduct( v, left ) / scale;
	out[2] = v[2] / scale;
}

// fraction of a ray from start along dir (unit vector) that is clear
static float Feeler( const vec3_t start, const vec3_t dir ) {
	trace_t tr;
	vec3_t end;
	VectorMA( start, GS_OBS_RAY_LENGTH, dir, end );
	GS_Trace( &tr, start, NULL, NULL, end, GS_PLAYER_NUM, MASK_PLAYERSOLID );
	return tr.fraction;
}

// which route target the player is heading for
static int RouteTarget( const gs_env_t *env ) {
	if ( env->routeTarget >= 0 ) {
		return env->routeTarget;
	}
	return env->timerState == GS_TIMER_IDLE ? GS_ROUTE_START : GS_ROUTE_FINISH;
}

static void Observe( const gs_env_t *env, float *o ) {
	const playerState_t *ps = &env->ps;
	vec3_t forward, left, dir, ahead[4], rel;
	int k = 0;

	YawAxes( ps->viewangles[YAW], forward, left );

	// motion
	ToPlayerFrame( ps->velocity, forward, left, 800, &o[k] );
	k += 3;
	o[k++] = sqrtf( ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1] ) / 800;
	o[k++] = ps->groundEntityNum != ENTITYNUM_NONE;
	o[k++] = ( ps->pm_flags & PMF_JUMP_HELD ) != 0;
	o[k++] = ps->powerups[PW_HASTE] != 0;

	// feelers: 16 level, 8 angled down, then down and up
	for ( int i = 0; i < 16; i++ ) {
		float a = DEG2RAD( ps->viewangles[YAW] + i * 22.5f );
		VectorSet( dir, cosf( a ), sinf( a ), 0 );
		o[k++] = Feeler( ps->origin, dir );
	}
	for ( int i = 0; i < 8; i++ ) {
		float a = DEG2RAD( ps->viewangles[YAW] + i * 45.0f );
		VectorSet( dir, cosf( a ) * M_SQRT1_2, sinf( a ) * M_SQRT1_2, -M_SQRT1_2 );
		o[k++] = Feeler( ps->origin, dir );
	}
	VectorSet( dir, 0, 0, -1 );
	o[k++] = Feeler( ps->origin, dir );
	VectorSet( dir, 0, 0, 1 );
	o[k++] = Feeler( ps->origin, dir );

	// the route ahead
	int found = GS_RouteAheadMany( RouteTarget( env ), ps->origin, routeAhead, 4, ahead ) == 0;
	for ( int i = 0; i < 4; i++ ) {
		if ( found ) {
			VectorSubtract( ahead[i], ps->origin, rel );
			ToPlayerFrame( rel, forward, left, 512, &o[k] );
		} else {
			o[k] = o[k + 1] = o[k + 2] = 0;
		}
		k += 3;
	}
	o[k++] = env->timerState == GS_TIMER_RUNNING;

	// weapons and health
	o[k++] = AngleNormalize180( ps->viewangles[PITCH] ) / 90;
	o[k++] = ps->stats[STAT_HEALTH] / 100.0f;
	o[k++] = ps->stats[STAT_ARMOR] / 100.0f;
	o[k++] = ps->ammo[WP_ROCKET_LAUNCHER] >= 10 ? 1 : ps->ammo[WP_ROCKET_LAUNCHER] / 10.0f;
	o[k++] = ps->weapon == WP_ROCKET_LAUNCHER && ps->weaponstate == WEAPON_READY && ps->weaponTime <= 0;
	o[k++] = ps->weaponTime > 0 ? ps->weaponTime / 1000.0f : 0;
}

typedef struct {
	gs_env_t	**envs;
	float		*obs;
	gs_state_t	*states;
	float		*routeDist;
} observeJob_t;

static void ObserveOne( int i, void *ctx ) {
	observeJob_t *job = ctx;
	const gs_env_t *env = job->envs[i];
	Observe( env, &job->obs[ i * GS_OBS_SIZE ] );
	if ( job->states ) {
		gs_env_get_state( env, &job->states[i] );
	}
	if ( job->routeDist ) {
		job->routeDist[i] = gs_route_distance( RouteTarget( env ), env->ps.origin );
	}
}

void gs_observe_batch( gs_env_t **envs, int n, int nthreads, float *obs, gs_state_t *states, float *route_dist ) {
	observeJob_t job = { envs, obs, states, route_dist };
	GS_ParallelFor( n, nthreads, ObserveOne, &job );
}
