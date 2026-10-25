// sim_local.h -- internal state shared by the engine side (env.c, entities.c, world.c,
// pool.c) and the game side (game.c)

#ifndef SIM_LOCAL_H
#define SIM_LOCAL_H

#include "q_shared.h"
#include "qcommon.h"
#include "bg_public.h"
#include "gauntlet.h"

#define GS_PLAYER_NUM		0
#define GS_FIRST_ENTITY		MAX_CLIENTS		// map entities get numbers from here, like g_entities
#define GS_MAX_ENTITIES		(MAX_GENTITIES - MAX_CLIENTS)
#define GS_MAX_TARGETS		8
#define GS_MAX_ITEMS		32		// powerup items tracked per env
#define GS_ITEM_HIDDEN		0x7fffffff	// itemReady value: not present until used

typedef enum {
	GE_IGNORED,				// not relevant to movement (lights, info_null, ...)
	GE_UNSUPPORTED,			// could affect movement, not simulated yet
	GE_WORLDSPAWN,
	GE_SPAWN,				// info_player_deathmatch / info_player_start
	GE_SOLID,				// func_static and other non-moving solid brush models
	GE_TRIGGER_MULTIPLE,
	GE_TRIGGER_TELEPORT,
	GE_TRIGGER_PUSH,
	GE_TRIGGER_HURT,
	GE_TARGET_POSITION,		// target_position, misc_teleporter_dest, info_notnull
	GE_TARGET_START_TIMER,
	GE_TARGET_STOP_TIMER,
	GE_TARGET_CHECKPOINT,
	GE_TARGET_KILL,
	GE_TARGET_TELEPORTER,
	GE_TARGET_PUSH,
	GE_ITEM_POWERUP,		// item_haste, item_flight
} gsEntType_t;

typedef struct {
	gsEntType_t		type;
	char			classname[64];
	char			target[64];
	char			targetname[64];
	int				spawnflags;
	int				dmg;
	int				count;
	float			wait;
	float			speed;
	vec3_t			origin;
	vec3_t			angles;

	// brush model entities (triggers, solids)
	clipHandle_t	model;			// 0 if none
	vec3_t			mins, maxs;		// model bounds (r.mins / r.maxs)
	vec3_t			absmin, absmax;	// linked bounds, padded by 1 like SV_LinkEntity
	int				contents;

	// resolved targets (indices into gs_world.ents)
	int				targets[GS_MAX_TARGETS];
	int				numTargets;

	vec3_t			pushVelocity;	// trigger_push / target_push: s.origin2

	// powerup items
	int				powerup;		// PW_HASTE, PW_FLIGHT
	int				quantity;		// seconds given per pickup
	int				itemSlot;		// index into gs_env.itemReady
} gsEnt_t;

typedef struct {
	int				loaded;
	char			mapname[MAX_QPATH];
	gsEnt_t			ents[GS_MAX_ENTITIES];
	int				numEnts;
	int				spawnEnt;		// index of the spawn point used, -1 if none
	int				numUnsupported;
	int				hasTimer;
	int				numItems;
} gsWorld_t;

extern gsWorld_t gs_world;

// one simulated player
struct gs_env {
	playerState_t	ps;
	usercmd_t		lastCmd;		// pers.cmd: last command received
	int				levelTime;		// server time of the last processed command
	int				frame;			// commands processed since spawn

	int				timerState;		// GS_TIMER_*
	int				timerStart;		// commandTime when the start trigger was last touched
	int				timerMs;
	int				checkpointMask[GS_MAX_ENTITIES / 32 + 1];
	int				checkpoints;
	int				dead;

	// item state is per player (each env races alone): the level time from
	// which each powerup item can be picked up again
	int				itemReady[GS_MAX_ITEMS];
};

// env.c
void	GS_SetError( const char *fmt, ... ) __attribute__ (( format( printf, 1, 2 ) ));

// entities.c
int		GS_ParseEntities( void );

// world.c
void	GS_Trace( trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs,
				  const vec3_t end, int passEntityNum, int contentMask );
int		GS_PointContents( const vec3_t point, int passEntityNum );
int		GS_EntitiesInBox( const vec3_t mins, const vec3_t maxs, int *list, int maxcount );
int		GS_EntityContact( const vec3_t mins, const vec3_t maxs, const gsEnt_t *ent );

// game.c (compiled with QVM float semantics)
void	G_InitEntities( void );
void	G_SpawnPlayer( gs_env_t *env );
void	G_PlacePlayer( gs_env_t *env, const vec3_t origin, const vec3_t velocity, float yaw );
void	G_ClientThink( gs_env_t *env, const usercmd_t *cmd );

#endif
