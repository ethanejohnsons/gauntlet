// env.c -- the public API: loading a map, map info, and creating, stepping and
// saving envs. The work happens in entities.c, world.c, game.c and pool.c.

#include <setjmp.h>
#include <stdarg.h>

#include "sim_local.h"
#include "pk3.h"
#include "q3_shim.h"

gsWorld_t gs_world;
static char gs_error[1024];
static char gs_source[512];

const char *gs_last_error( void ) {
	return gs_error;
}

void GS_SetError( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( gs_error, sizeof( gs_error ), fmt, ap );
	va_end( ap );
}

void gs_set_verbose( int level ) {
	gs_verbose = level;
}

// ---------------------------------------------------------------------------
// map loading

int gs_load_map( const char *basepath, const char *game, const char *map ) {
	char name[MAX_QPATH];
	void *data;
	int len;
	jmp_buf jb;

	gs_error[0] = 0;
	snprintf( name, sizeof( name ), "maps/%s.bsp", map );
	if ( !pk3_read_file( basepath, game, name, &data, &len, gs_source, sizeof( gs_source ) ) ) {
		GS_SetError( "%s not found in %s/%s or %s/baseq3", name, basepath, game, basepath );
		return -1;
	}

	if ( setjmp( jb ) ) {
		GS_SetErrorJump( NULL );
		GS_SetError( "%s", GS_LastError() );
		gs_world.loaded = 0;
		return -1;
	}
	GS_SetErrorJump( &jb );

	gs_world.loaded = 0;
	GS_HunkClear();
	GS_SetPendingFile( data, len );
	int checksum;
	CM_LoadMap( name, qfalse, &checksum );
	GS_BumpMapGeneration();
	GS_SetErrorJump( NULL );

	Q_strncpyz( gs_world.mapname, map, sizeof( gs_world.mapname ) );
	if ( GS_ParseEntities() < 0 ) {
		return -1;
	}
	G_InitEntities();
	gs_world.loaded = 1;
	return 0;
}

void gs_map_info( gs_map_info_t *info ) {
	memset( info, 0, sizeof( *info ) );
	if ( !gs_world.loaded ) {
		return;
	}
	Q_strncpyz( info->mapname, gs_world.mapname, sizeof( info->mapname ) );
	Q_strncpyz( info->source, gs_source, sizeof( info->source ) );
	info->num_entities = gs_world.numEnts;
	info->num_unsupported = gs_world.numUnsupported;
	info->has_timer = gs_world.hasTimer;
	if ( gs_world.spawnEnt >= 0 ) {
		VectorCopy( gs_world.ents[ gs_world.spawnEnt ].origin, info->spawn_origin );
		VectorCopy( gs_world.ents[ gs_world.spawnEnt ].angles, info->spawn_angles );
	}
	CM_ModelBounds( 0, info->world_mins, info->world_maxs );
}

// ---------------------------------------------------------------------------
// envs

gs_env_t *gs_env_new( void ) {
	gs_env_t *env = calloc( 1, sizeof( *env ) );
	return env;
}

void gs_env_free( gs_env_t *env ) {
	free( env );
}

void gs_env_spawn( gs_env_t *env ) {
	G_SpawnPlayer( env );
}

void gs_env_step( gs_env_t *env, const gs_cmd_t *in ) {
	usercmd_t cmd;
	memset( &cmd, 0, sizeof( cmd ) );
	// the client sends one command per 8 ms frame (com_maxfps 125)
	cmd.serverTime = env->ps.commandTime + GS_FRAME_MSEC;
	cmd.angles[0] = in->angles[0];
	cmd.angles[1] = in->angles[1];
	cmd.angles[2] = in->angles[2];
	cmd.forwardmove = in->forward;
	cmd.rightmove = in->right;
	cmd.upmove = in->up;
	cmd.buttons = in->buttons;
	cmd.weapon = env->ps.weapon;
	G_ClientThink( env, &cmd );
	env->frame++;
}

void gs_env_place( gs_env_t *env, const float origin[3], const float velocity[3], float yaw ) {
	G_PlacePlayer( env, origin, velocity, yaw );
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

