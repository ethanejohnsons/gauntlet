// sim_local.h -- internal state shared by env.c (engine side) and game.c (game side)

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
} gsEntType_t;

typedef struct {
	gsEntType_t		type;
	char			classname[64];
	char			target[64];
	char			targetname[64];
	int				spawnflags;
	int				dmg;
	float			wait;
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

	vec3_t			pushVelocity;	// trigger_push: s.origin2 from AimAtTarget
} gsEnt_t;

typedef struct {
	int				loaded;
	char			mapname[MAX_QPATH];
	gsEnt_t			ents[GS_MAX_ENTITIES];
	int				numEnts;
	int				spawnEnt;		// index of the spawn point used, -1 if none
	int				numUnsupported;
	int				hasTimer;
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

	int				touchedStart;	// set during a step if the start trigger was touched
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
void	G_InitTriggers( void );
void	G_SpawnPlayer( gs_env_t *env );
void	G_ClientThink( gs_env_t *env, const usercmd_t *cmd );

#endif
