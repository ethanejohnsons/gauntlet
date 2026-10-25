// game.c -- the parts of the Quake III game module that move a DeFRaG player:
// spawning, ClientThink, trigger touches, teleports, jump pads and DeFRaG's timer
// targets. Rewritten from id's 1.32 g_active.c / g_client.c / g_trigger.c / g_misc.c.
// DeFRaG's own game code is closed source, so the timer behavior is our best
// reading of it and is checked against the real mod (M2).
//
// Compiled like the QVM (see bg_vmcompat.h), because in the real game this all
// runs inside DeFRaG's qagame.qvm.

#include "sim_local.h"

static const vec3_t	playerMins = { -15, -15, -24 };
static const vec3_t	playerMaxs = { 15, 15, 32 };

#define DEFAULT_GRAVITY		800
#define DEFAULT_SPEED		320

/*
=================
AimAtTarget (g_trigger.c)

Calculate the jump pad velocity needed to hit the target.
=================
*/
static void AimAtTarget( gsEnt_t *self ) {
	gsEnt_t		*ent;
	vec3_t		origin;
	float		height, gravity, time, forward;
	float		dist;

	VectorAdd( self->absmin, self->absmax, origin );
	VectorScale ( origin, 0.5, origin );

	if ( !self->numTargets ) {
		self->type = GE_IGNORED;	// G_FreeEntity
		return;
	}
	ent = &gs_world.ents[ self->targets[0] ];	// G_PickTarget: only one target in practice

	height = ent->origin[2] - origin[2];
	gravity = DEFAULT_GRAVITY;
	time = sqrt( height / ( .5 * gravity ) );
	if ( !time ) {
		self->type = GE_IGNORED;
		return;
	}

	// set s.origin2 to the push velocity
	VectorSubtract ( ent->origin, origin, self->pushVelocity );
	self->pushVelocity[2] = 0;
	dist = VectorNormalize( self->pushVelocity );

	forward = dist / time;
	VectorScale( self->pushVelocity, forward, self->pushVelocity );

	self->pushVelocity[2] = time * gravity;
}

void G_InitTriggers( void ) {
	for ( int i = 0; i < gs_world.numEnts; i++ ) {
		if ( gs_world.ents[i].type == GE_TRIGGER_PUSH ) {
			AimAtTarget( &gs_world.ents[i] );
		}
	}
}

/*
=================
SetClientViewAngle (g_client.c)
=================
*/
static void SetClientViewAngle( gs_env_t *env, const vec3_t angle ) {
	int			i;

	// set the delta angle
	for (i=0 ; i<3 ; i++) {
		int		cmdAngle;

		cmdAngle = ANGLE2SHORT(angle[i]);
		env->ps.delta_angles[i] = cmdAngle - env->lastCmd.angles[i];
	}
	VectorCopy (angle, env->ps.viewangles);
}

/*
=================
TeleportPlayer (g_misc.c)
=================
*/
static void TeleportPlayer( gs_env_t *env, const vec3_t origin, const vec3_t angles, int keepSpeed ) {
	playerState_t *ps = &env->ps;
	float speed = VectorLength( ps->velocity );

	VectorCopy ( origin, ps->origin );
	ps->origin[2] += 1;

	// spit the player out
	AngleVectors( angles, ps->velocity, NULL, NULL );
	// DeFRaG KEEP_SPEED: "stores the speed at entering the teleporter and restores it
	// at the destination". Exact behavior unverified (M2).
	VectorScale( ps->velocity, keepSpeed ? speed : 400, ps->velocity );
	ps->pm_time = 160;		// hold time
	ps->pm_flags |= PMF_TIME_KNOCKBACK;

	// toggle the teleport bit so the client knows to not lerp
	ps->eFlags ^= EF_TELEPORT_BIT;

	// set angles
	SetClientViewAngle( env, angles );
}

/*
=================
Target use: G_UseTargets + the DeFRaG timer targets
=================
*/
static void G_UseEntity( gs_env_t *env, gsEnt_t *ent );

static void G_UseTargets( gs_env_t *env, const gsEnt_t *ent ) {
	for ( int i = 0; i < ent->numTargets; i++ ) {
		G_UseEntity( env, &gs_world.ents[ ent->targets[i] ] );
	}
}

static void G_UseEntity( gs_env_t *env, gsEnt_t *ent ) {
	int index = ent - gs_world.ents;

	switch ( ent->type ) {
	case GE_TARGET_START_TIMER:
		// Touching the start zone keeps restarting the timer, so in effect the
		// run starts on the last frame inside it.
		env->timerState = GS_TIMER_RUNNING;
		env->timerStart = env->ps.commandTime;
		env->timerMs = 0;
		env->checkpoints = 0;
		memset( env->checkpointMask, 0, sizeof( env->checkpointMask ) );
		break;
	case GE_TARGET_STOP_TIMER:
		if ( env->timerState == GS_TIMER_RUNNING ) {
			env->timerState = GS_TIMER_FINISHED;
			env->timerMs = env->ps.commandTime - env->timerStart;
		}
		break;
	case GE_TARGET_CHECKPOINT:
		if ( env->timerState == GS_TIMER_RUNNING && !( env->checkpointMask[index >> 5] & ( 1 << ( index & 31 ) ) ) ) {
			env->checkpointMask[index >> 5] |= 1 << ( index & 31 );
			env->checkpoints++;
		}
		break;
	case GE_TARGET_KILL:
		env->dead = 1;
		break;
	case GE_TARGET_TELEPORTER:
		if ( ent->numTargets ) {
			const gsEnt_t *dest = &gs_world.ents[ ent->targets[0] ];
			TeleportPlayer( env, dest->origin, dest->angles, 0 );
		}
		break;
	default:
		break;
	}
}

/*
=================
G_TouchTriggers (g_active.c)
=================
*/
static void G_TouchTriggers( gs_env_t *env, const vec3_t rmins, const vec3_t rmaxs ) {
	int			i, num;
	int			touch[GS_MAX_ENTITIES];
	vec3_t		mins, maxs;
	static const vec3_t	range = { 40, 40, 52 };
	playerState_t *ps = &env->ps;

	// dead clients don't activate triggers!
	if ( env->dead ) {
		return;
	}

	VectorSubtract( ps->origin, range, mins );
	VectorAdd( ps->origin, range, maxs );

	num = GS_EntitiesInBox( mins, maxs, touch, GS_MAX_ENTITIES );

	// can't use ent->absmin, because that has a one unit pad
	VectorAdd( ps->origin, rmins, mins );
	VectorAdd( ps->origin, rmaxs, maxs );

	for ( i=0 ; i<num ; i++ ) {
		gsEnt_t *hit = &gs_world.ents[ touch[i] ];

		if ( !( hit->contents & CONTENTS_TRIGGER ) ) {
			continue;
		}
		if ( !GS_EntityContact( mins, maxs, hit ) ) {
			continue;
		}

		switch ( hit->type ) {
		case GE_TRIGGER_MULTIPLE:
			G_UseTargets( env, hit );
			break;
		case GE_TRIGGER_TELEPORT:
			if ( ps->pm_type == PM_DEAD || ( hit->spawnflags & 1 ) || !hit->numTargets ) {
				break;	// SPECTATOR teleporters are for spectators only
			}
			TeleportPlayer( env, gs_world.ents[ hit->targets[0] ].origin,
							gs_world.ents[ hit->targets[0] ].angles, hit->spawnflags & 2 );
			break;
		case GE_TRIGGER_PUSH: {
			entityState_t pad;
			memset( &pad, 0, sizeof( pad ) );
			pad.number = GS_FIRST_ENTITY + touch[i];
			VectorCopy( hit->pushVelocity, pad.origin2 );
			BG_TouchJumpPad( ps, &pad );
			break;
		}
		case GE_TRIGGER_HURT:
			// DeFRaG maps use trigger_hurt as kill zones; treated as instant death
			env->dead = 1;
			return;
		default:
			break;
		}
		if ( env->dead ) {
			return;
		}
	}

	// if we didn't touch a jump pad this pmove frame
	if ( ps->jumppad_frame != ps->pmove_framecount ) {
		ps->jumppad_frame = 0;
		ps->jumppad_ent = 0;
	}
}

/*
=================
G_ClientThink (ClientThink_real in g_active.c, with DeFRaG's pmove_fixed 1 / pmove_msec 8)
=================
*/
void G_ClientThink( gs_env_t *env, const usercmd_t *cmd ) {
	usercmd_t	ucmd = *cmd;
	pmove_t		pm;
	playerState_t *ps = &env->ps;

	// pmove_fixed: round the command time up to a whole frame
	ucmd.serverTime = ( ( ucmd.serverTime + GS_FRAME_MSEC - 1 ) / GS_FRAME_MSEC ) * GS_FRAME_MSEC;

	ps->pm_type = env->dead ? PM_DEAD : PM_NORMAL;
	ps->gravity = DEFAULT_GRAVITY;
	ps->speed = DEFAULT_SPEED;

	memset( &pm, 0, sizeof( pm ) );
	pm.ps = ps;
	pm.cmd = ucmd;
	pm.tracemask = ps->pm_type == PM_DEAD ? MASK_PLAYERSOLID & ~CONTENTS_BODY : MASK_PLAYERSOLID;
	pm.trace = GS_Trace;
	pm.pointcontents = GS_PointContents;
	pm.pmove_fixed = 1;
	pm.pmove_msec = GS_FRAME_MSEC;

	Pmove( &pm );

	env->lastCmd = *cmd;
	env->levelTime = ucmd.serverTime;

	// lava and slime kill, like a death pit
	if ( pm.watertype & ( CONTENTS_LAVA | CONTENTS_SLIME ) ) {
		env->dead = 1;
	}

	G_TouchTriggers( env, pm.mins, pm.maxs );

	if ( env->timerState == GS_TIMER_RUNNING ) {
		env->timerMs = ps->commandTime - env->timerStart;
	}
}

/*
=================
G_SpawnPlayer (ClientSpawn in g_client.c, movement-relevant parts)
=================
*/
void G_SpawnPlayer( gs_env_t *env ) {
	vec3_t		spawn_origin, spawn_angles;
	usercmd_t	cmd;
	playerState_t *ps = &env->ps;

	memset( env, 0, sizeof( *env ) );

	VectorClear( spawn_origin );
	VectorClear( spawn_angles );
	if ( gs_world.spawnEnt >= 0 ) {
		VectorCopy( gs_world.ents[ gs_world.spawnEnt ].origin, spawn_origin );
		VectorCopy( gs_world.ents[ gs_world.spawnEnt ].angles, spawn_angles );
	}
	spawn_origin[2] += 9;

	ps->clientNum = GS_PLAYER_NUM;
	ps->stats[STAT_MAX_HEALTH] = 100;
	ps->stats[STAT_HEALTH] = 125;
	ps->stats[STAT_WEAPONS] = ( 1 << WP_MACHINEGUN ) | ( 1 << WP_GAUNTLET );
	ps->ammo[WP_MACHINEGUN] = 100;
	ps->ammo[WP_GAUNTLET] = -1;
	ps->weapon = WP_MACHINEGUN;
	ps->weaponstate = WEAPON_READY;
	ps->groundEntityNum = ENTITYNUM_NONE;

	VectorCopy( spawn_origin, ps->origin );

	// the respawned flag will be cleared after the attack and jump keys come up
	ps->pm_flags |= PMF_RESPAWNED;
	SetClientViewAngle( env, spawn_angles );

	// don't allow full run speed for a bit
	ps->pm_flags |= PMF_TIME_KNOCKBACK;
	ps->pm_time = 100;

	// run a client frame to drop exactly to the floor
	env->levelTime = 1000;
	ps->commandTime = env->levelTime - 100;
	memset( &cmd, 0, sizeof( cmd ) );
	cmd.serverTime = env->levelTime;
	G_ClientThink( env, &cmd );
	env->frame = 0;
}
