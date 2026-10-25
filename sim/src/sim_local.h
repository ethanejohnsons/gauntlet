// sim_local.h -- everything the sim's own source files share.
//
// Files:
//   env.c      the players: the public env API, and the thread pool for batches
//   observe.c  what a player senses, for training (gs_observe_batch)
//   world.c    the map: loading, entity parsing, and the world queries pmove uses
//   route.c    the route: every open place's distance to the finish
//   game.c     game logic from Q3's game module: spawn, think, triggers, items, timer
//   combat.c   the rocket launcher: firing, rockets in flight, splash damage, knockback
//   pk3.c      reading files out of pk3 (zip) archives
//   q3_shim.c  stand-ins for the engine services the vendored Q3 code calls
//
// Naming: gs_* is the public API (gauntlet.h), GS_* is internal to the sim, and
// G_* / SP_* / Touch_* in game.c keep the names of the Q3 functions they mirror.
//
// game.c is compiled like the QVM (see q3/game/bg_vmcompat.h); the rest is engine
// code compiled normally.

#ifndef SIM_LOCAL_H
#define SIM_LOCAL_H

#include <limits.h>
#include <setjmp.h>

#include "q_shared.h"
#include "qcommon.h"
#include "bg_public.h"
#include "gauntlet.h"

#define GS_PLAYER_NUM		0
#define GS_FIRST_ENTITY		MAX_CLIENTS		// map entities are numbered from here, like g_entities
#define GS_MAX_ENTITIES		(MAX_GENTITIES - MAX_CLIENTS)
#define GS_MAX_TARGETS		8
#define GS_MAX_ITEMS		32				// items tracked per env
#define GS_MAX_MISSILES		8				// rockets in flight per env
#define GS_MAX_CHECKPOINTS	64				// checkpoint targets tracked per env
#define GS_ROUTE_MAX_TARGETS	( GS_ROUTE_CHECKPOINT0 + GS_MAX_CHECKPOINTS )
#define GS_ITEM_HIDDEN		INT_MAX			// itemReady value: not there until something uses it

// ---------------------------------------------------------------------------
// the map (world.c)

typedef enum {
	GE_IGNORED,				// no effect on movement (lights, models, weapons, ...)
	GE_UNSUPPORTED,			// could affect movement, not simulated yet
	GE_SPAWN,				// info_player_deathmatch / info_player_start
	GE_SOLID,				// func_static: a solid brush model that never moves
	GE_TRIGGER_MULTIPLE,	// fires its targets while touched
	GE_TRIGGER_TELEPORT,
	GE_TRIGGER_PUSH,		// jump pad
	GE_TRIGGER_HURT,		// kill zone
	GE_TARGET_POSITION,		// target_position, misc_teleporter_dest, info_notnull
	GE_TARGET_START_TIMER,
	GE_TARGET_STOP_TIMER,
	GE_TARGET_CHECKPOINT,
	GE_TARGET_KILL,
	GE_TARGET_TELEPORTER,
	GE_TARGET_PUSH,
	GE_TARGET_GIVE,			// gives its target items to the player
	GE_ITEM,				// a pickup the sim simulates: haste, flight, battlesuit, rocket launcher, rockets
} gsEntType_t;

typedef struct {
	gsEntType_t		type;
	char			classname[64];
	char			target[64];
	char			targetname[64];
	int				spawnflags;
	int				count;
	float			wait;
	float			speed;
	vec3_t			origin;
	vec3_t			angles;

	// linked entities (brush models and items) can be touched or traced against
	clipHandle_t	model;			// brush model, 0 if none
	vec3_t			mins, maxs;		// bounds relative to origin (r.mins / r.maxs)
	vec3_t			absmin, absmax;	// world bounds, padded by 1 like SV_LinkEntity
	int				contents;		// 0 if not linked

	int				targets[GS_MAX_TARGETS];	// indices of the entities `target` names
	int				numTargets;

	vec3_t			pushVelocity;	// trigger_push / target_push: launch velocity (s.origin2)

	const gitem_t	*item;			// items: what it is (bg_itemlist)
	int				quantity;		// items: seconds of powerup, or ammo, per pickup
	int				slot;			// items and checkpoints: index into the env's per-player state
} gsEnt_t;

typedef struct {
	int				loaded;
	char			mapname[MAX_QPATH];
	char			source[512];	// pk3 the BSP came from
	gsEnt_t			ents[GS_MAX_ENTITIES];
	int				numEnts;
	int				spawnEnt;		// index of the spawn point used, -1 if none
	int				numUnsupported;
	int				numItems;
	int				numCheckpoints;
	int				hasTimer;		// has both start and stop timer targets
} gsWorld_t;

extern gsWorld_t gs_world;

void	GS_Trace( trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs,
				  const vec3_t end, int passEntityNum, int contentMask );
int		GS_PointContents( const vec3_t point, int passEntityNum );
int		GS_EntitiesInBox( const vec3_t mins, const vec3_t maxs, int *list, int maxcount );
int		GS_EntityContact( const vec3_t mins, const vec3_t maxs, const gsEnt_t *ent );
void	GS_SetError( const char *fmt, ... ) __attribute__ (( format( printf, 1, 2 ) ));

// ---------------------------------------------------------------------------
// the players (env.c)

// a rocket in flight (fire_rocket in g_missile.c)
typedef struct {
	int				active;
	trajectory_t	pos;			// TR_LINEAR from the muzzle at 900 ups
	vec3_t			currentOrigin;
	int				explodeTime;	// level time it explodes by itself (15 s after firing)
} gsMissile_t;

struct gs_env {
	playerState_t	ps;
	usercmd_t		lastCmd;		// the last command received (pers.cmd)
	int				levelTime;		// server time of the last processed command
	int				frame;			// commands processed since spawn

	int				timerState;		// GS_TIMER_*
	int				timerStart;		// commandTime when the player last entered the start zone
	int				inStartZone;	// touching a start trigger after the last step
	int				touchedStart;	// touching one during this step (G_TouchTriggers)
	int				timerMs;
	int				checkpoints;	// distinct checkpoints touched since the timer started
	char			checkpointSeen[GS_MAX_CHECKPOINTS];
	int				dead;
	int				routeTarget;	// gs_env_set_route_target; -1: by the timer

	// Items are per player, since each env races alone: the level time from which
	// each item can be picked up again.
	int				itemReady[GS_MAX_ITEMS];

	// combat (combat.c)
	int				timeResidual;	// ms towards the next once-a-second health/armor tick
	vec3_t			linkMins;		// the player's linked bounds (absmin / absmax), from
	vec3_t			linkMaxs;		// the snapped origin, as the server last linked it
	gsMissile_t		missiles[GS_MAX_MISSILES];
};

// run fn( i, ctx ) for every i < n, spread over nthreads threads
void	GS_ParallelFor( int n, int nthreads, void ( *fn )( int i, void *ctx ), void *ctx );

// ---------------------------------------------------------------------------
// game logic (game.c)

void	G_InitEntities( void );
void	G_SpawnPlayer( gs_env_t *env );
void	G_PlacePlayer( gs_env_t *env, const vec3_t origin, const vec3_t velocity, float yaw );
void	G_ClientThink( gs_env_t *env, const usercmd_t *cmd );

// combat.c (compiled with QVM float semantics too)
#define DAMAGE_RADIUS			0x00000001	// damage was indirect (g_local.h)
#define DAMAGE_NO_ARMOR			0x00000002	// armour does not protect from this damage
#define DAMAGE_NO_KNOCKBACK		0x00000004	// do not affect velocity, just view angles

void	G_FireWeapon( gs_env_t *env, const vec3_t trBase );
void	G_RunMissiles( gs_env_t *env, int levelTime );
void	G_Damage( gs_env_t *env, vec3_t dir, int damage, int dflags, int selfInflicted );

// ---------------------------------------------------------------------------
// pk3.c

// Read `name` (e.g. "maps/vanilla_01.bsp") from <basepath>/<game>/*.pk3, falling back
// to <basepath>/baseq3/*.pk3. Returns 1 and a malloc'd buffer on success, and the
// archive's path in `srcpk3`.
int		GS_ReadPk3File( const char *basepath, const char *game, const char *name,
						void **out, int *outlen, char *srcpk3, size_t srclen );

// ---------------------------------------------------------------------------
// q3_shim.c

extern int gs_verbose;

void	GS_SetErrorJump( jmp_buf *jb );		// Com_Error longjmps here (NULL: abort)
void	GS_SetPendingFile( void *data, int len );	// what the next FS_ReadFile returns
void	GS_HunkClear( void );				// free everything the last map allocated
void	GS_BumpMapGeneration( void );		// a new map is loaded: reallocate visit stamps

#endif
