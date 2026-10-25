// gauntlet.h -- libgauntlet: headless Quake III / DeFRaG (VQ3) movement simulation
//
// One map is loaded per process. Any number of players (envs) can be stepped on it,
// in parallel with gs_step_batch. Physics run in fixed 8 ms frames, like DeFRaG's
// pmove_fixed 1 / pmove_msec 8.

#ifndef GAUNTLET_H
#define GAUNTLET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GS_FRAME_MSEC		8

#define GS_BUTTON_ATTACK	1
#define GS_BUTTON_WALKING	16

#define GS_TIMER_IDLE		0	// not started
#define GS_TIMER_RUNNING	1
#define GS_TIMER_FINISHED	2

// One frame of input, exactly as the client sends it (usercmd_t).
// angles are raw 16-bit client angles (ANGLE2SHORT of the client's view angles).
// The view is angles + delta_angles, so the same cmd stream replays identically in oDFe.
typedef struct {
	int32_t	angles[3];		// pitch, yaw, roll; only the low 16 bits matter
	int8_t	forward;		// -127, 0, 127 on a keyboard
	int8_t	right;
	int8_t	up;				// jump > 0, crouch < 0
	uint8_t	buttons;
} gs_cmd_t;

typedef struct {
	float	origin[3];
	float	velocity[3];
	float	viewangles[3];
	int32_t	delta_angles[3];
	int32_t	command_time;	// ms
	int32_t	pm_flags;
	int32_t	pm_time;
	int32_t	ground_entity;	// -1 when airborne
	int32_t	frame;			// commands processed since spawn
	int32_t	timer_state;	// GS_TIMER_*
	int32_t	timer_ms;		// elapsed (running) or final (finished)
	int32_t	checkpoints;	// distinct checkpoints touched since the timer started
	int32_t	dead;
} gs_state_t;

typedef struct {
	char	mapname[64];
	char	source[512];		// pk3 the BSP came from
	int32_t	num_entities;
	int32_t	num_unsupported;	// entities that could affect movement but aren't simulated
	int32_t	has_timer;			// has start and stop timer targets
	float	spawn_origin[3];
	float	spawn_angles[3];
	float	world_mins[3];
	float	world_maxs[3];
} gs_map_info_t;

typedef struct gs_env gs_env_t;

// Load maps/<map>.bsp from <basepath>/<game>/*.pk3 or <basepath>/baseq3/*.pk3.
// Returns 0 on success, -1 on error (see gs_last_error).
int			gs_load_map( const char *basepath, const char *game, const char *map );
const char	*gs_last_error( void );
void		gs_map_info( gs_map_info_t *info );
// Name and reason for each unsupported entity, one per line. Returns length written.
int			gs_unsupported_entities( char *buf, int buflen );
void		gs_set_verbose( int level );

gs_env_t	*gs_env_new( void );
void		gs_env_free( gs_env_t *env );
void		gs_env_spawn( gs_env_t *env );			// at the map's spawn point
void		gs_env_step( gs_env_t *env, const gs_cmd_t *cmd );
void		gs_env_get_state( const gs_env_t *env, gs_state_t *out );

// Whole-env snapshots, for start-state resets. Plain memory, safe to copy.
size_t		gs_env_snapshot_size( void );
void		gs_env_save( const gs_env_t *env, void *buf );
void		gs_env_restore( gs_env_t *env, const void *buf );

// Step envs[i] with cmds[i] for i < n, across nthreads worker threads (<= 1: inline).
void		gs_step_batch( gs_env_t **envs, const gs_cmd_t *cmds, int n, int nthreads );

// World trace for observations: fraction along start->end for a box (mins/maxs may be NULL).
float		gs_trace( const float start[3], const float end[3], const float mins[3],
					  const float maxs[3], float endpos[3] );

#ifdef __cplusplus
}
#endif

#endif
