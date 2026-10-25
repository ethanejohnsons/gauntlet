// gauntlet.h -- libgauntlet: headless Quake III / DeFRaG (VQ3) movement simulation
//
// Typical use:
//   gs_load_map( "~/Games/quake3", "defrag", "vanilla_01" );
//   gs_env_t *env = gs_env_new();
//   gs_env_spawn( env );
//   for each frame: gs_env_step( env, &cmd ); gs_env_get_state( env, &state );
//
// One map is loaded per process (Q3's collision code keeps it in a global). Any
// number of players (envs) can be stepped on it, in parallel with gs_step_batch.
// Physics run in fixed 8 ms frames, like DeFRaG's pmove_fixed 1 / pmove_msec 8.

#ifndef GAUNTLET_H
#define GAUNTLET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GS_FRAME_MSEC		8

#define GS_BUTTON_ATTACK	1		// gs_cmd_t.buttons: fire
#define GS_BUTTON_WALKING	16

#define GS_WP_MACHINEGUN	2		// gs_cmd_t.weapon (WP_* in bg_public.h)
#define GS_WP_ROCKET_LAUNCHER	5	// the only weapon the sim fires

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
	uint8_t	buttons;		// GS_BUTTON_*
	uint8_t	weapon;			// weapon to hold (GS_WP_*); 0: keep the current one
	uint8_t	unused[3];
} gs_cmd_t;

// What a player looks like after a step.
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
	int32_t	haste_ms;		// haste time left, 0 if none
	int32_t	flight_ms;
	int32_t	health;
	int32_t	armor;
	int32_t	weapon;			// weapon held (GS_WP_*)
	int32_t	weapons;		// weapons owned, bit (1 << GS_WP_*)
	int32_t	rockets;		// rocket launcher ammo
	int32_t	weapon_time;	// ms until the held weapon can fire again
	int32_t	rockets_in_flight;
} gs_state_t;

// Facts about the loaded map.
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

// A map entity, for finding start/finish zones, checkpoints and the like.
typedef struct {
	char	classname[64];
	char	target[64];
	char	targetname[64];
	float	origin[3];
	float	absmin[3];		// linked bounds (brush models and items), else the origin
	float	absmax[3];
	int32_t	supported;		// 0 if it could affect movement but isn't simulated
} gs_entity_t;

typedef struct gs_env gs_env_t;

// --- the map ---------------------------------------------------------------

// Load maps/<map>.bsp from <basepath>/<game>/*.pk3 or <basepath>/baseq3/*.pk3.
// Returns 0 on success, -1 on error (see gs_last_error).
int			gs_load_map( const char *basepath, const char *game, const char *map );
const char	*gs_last_error( void );
void		gs_map_info( gs_map_info_t *info );
int			gs_num_entities( void );
int			gs_entity( int index, gs_entity_t *out );	// 0 on success, -1 if out of range
// World trace, e.g. for observations: the fraction of start->end that is clear, and
// where it stopped. mins/maxs give a box (NULL for a point); endpos may be NULL.
float		gs_trace( const float start[3], const float end[3], const float mins[3],
					  const float maxs[3], float endpos[3] );
void		gs_set_verbose( int level );		// 1: print Q3's messages, 2: also debug ones

// --- the route: distance to the start zone and to the finish -----------------
//
// A grid over the map holding, for every place a player fits, the length of the
// shortest path through open space to a target: the start zone (which starts the
// timer) or the finish. Gravity is ignored (the path floats over gaps) and
// teleporters count as free jumps. Used for rewards ("how much closer did I get?")
// and to show the way ahead.

#define GS_ROUTE_START		0
#define GS_ROUTE_FINISH		1
#define GS_ROUTE_TARGETS	2

// Build it for the loaded map; `cell` is the grid spacing in units (<= 0: 16). Takes
// a few seconds. Returns 0, or -1 if the map has no start or finish (see gs_last_error).
int			gs_route_build( float cell );
// Distance from pos to the target (GS_ROUTE_*), or -1 if pos isn't connected to it.
float		gs_route_distance( int target, const float pos[3] );
// The point about `ahead` units further along the route to the target. Returns 0, or -1.
int			gs_route_ahead( int target, const float pos[3], float ahead, float out[3] );

// --- players ---------------------------------------------------------------

gs_env_t	*gs_env_new( void );
void		gs_env_free( gs_env_t *env );
void		gs_env_spawn( gs_env_t *env );		// (re)start at the map's spawn point
void		gs_env_step( gs_env_t *env, const gs_cmd_t *cmd );	// one 8 ms frame
void		gs_env_get_state( const gs_env_t *env, gs_state_t *out );
// Put the player somewhere, like DeFRaG's placeplayer: origin, velocity and view yaw.
// The timer, items and everything else are kept.
void		gs_env_place( gs_env_t *env, const float origin[3], const float velocity[3], float yaw );

// Whole-env snapshots, for restarting from a saved moment. Plain memory, safe to copy.
size_t		gs_env_snapshot_size( void );
void		gs_env_save( const gs_env_t *env, void *buf );
void		gs_env_restore( gs_env_t *env, const void *buf );

// Step envs[i] with cmds[i] for every i < n, spread over nthreads threads (the
// caller is one of them; <= 1 steps them all on the calling thread). Results are
// identical whatever the thread count.
void		gs_step_batch( gs_env_t **envs, const gs_cmd_t *cmds, int n, int nthreads );

// --- observations, for training --------------------------------------------
//
// What a player senses, as GS_OBS_SIZE floats, roughly in [-1, 1]. Directions are
// relative to where the player looks (yaw only): x forward, y left, z up.
//
//   0-2    velocity (x, y, z) / 800
//   3      horizontal speed / 800
//   4      on the ground (0 or 1)
//   5      jump held: jump must be released before the next jump (0 or 1)
//   6      haste active (0 or 1)
//   7-22   16 level feelers, every 22.5 degrees starting straight ahead, turning left:
//          how far a ray from the player's origin gets before hitting something,
//          as a fraction of GS_OBS_RAY_LENGTH
//   23-30  8 feelers angled 45 degrees down, every 45 degrees (find edges and pits)
//   31     feeler straight down,  32  straight up
//   33-44  the route ahead: points 64, 128, 256 and 512 units further along the route,
//          relative to the player (x, y, z) / 512; zeros if there is no route here.
//          The route leads to the start zone until the timer runs, then to the finish.
//   45     timer running (0 or 1)
#define GS_OBS_SIZE			46
#define GS_OBS_RAY_LENGTH	1024

// Fill obs[i * GS_OBS_SIZE ...] for envs[i], and optionally states[i] (as
// gs_env_get_state) and route_dist[i] (gs_route_distance from the player's origin to
// the current target: the start zone until the timer runs, then the finish).
// states and route_dist may be NULL.
void		gs_observe_batch( gs_env_t **envs, int n, int nthreads, float *obs,
							  gs_state_t *states, float *route_dist );

#ifdef __cplusplus
}
#endif

#endif
