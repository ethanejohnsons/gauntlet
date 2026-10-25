// route.c -- the route: how far every point of the map is from the start zone and
// from the finish.
//
// gs_route_build covers the map with a grid of cubes ("cells"), marks the cells a
// (crouching) player fits in, and then works out, for every open cell, the length of
// the shortest path through open cells to the start zone and to the finish zone
// (Dijkstra's algorithm, once per target). Teleporters count as a free jump from the
// trigger to the destination.
//
// A DeFRaG run goes: the start zone (which starts the timer), the checkpoints, then
// the finish (which stops it). Each of those zones is a target.
//
// Gravity is only roughly accounted for, with two costs and one limit:
//  - climbing costs CLIMB_COST times as much as moving sideways or down, so the route
//    goes around tall walls rather than over them, and happily drops down holes;
//  - moving sideways through "air" (no floor within SUPPORT_HEIGHT below, or only
//    one inside a kill zone) costs AIR_COST times as much, so the route jumps gaps
//    when it has to, but takes the long way around a gap nobody could jump;
//  - on maps with few rockets (under PLENTY_ROCKETS in all), the route only climbs as
//    high as a jump reaches: JUMP_REACH above a floor under where it climbs from, and
//    not at all from over a pit. Without that it would climb out of places no player
//    can (tatmt-s2 hands out one rocket, and has a low ledge 64 units below the next
//    platform, across a kill pit, that the route used to lead straight into). With
//    rockets to spare, rocket jumps can climb anywhere (vanilla_01's finish is 1200
//    units up), and so can the route. In and above jump pads, climbing has no limit.
// That's what we want for a reward. It says which way is forward and how far is left,
// and leaves *how* to get there (jumping, strafing) to the player.

#include "sim_local.h"

#define DEFAULT_CELL	16.0f
#define UNREACHED		1e30f
#define CLIMB_COST		8.0f	// each unit up counts as this many units of distance
#define AIR_COST		12.0f	// each unit sideways with no floor below, likewise
#define SUPPORT_HEIGHT	96.0f	// floor this close below a cell makes it "not air"
#define JUMP_REACH		63.6f	// a jump lands on a floor this much higher: 45.6 up (JUMP_VELOCITY
								// 270 against gravity 800) plus an 18 unit step up
#define STAND_HEIGHT	24.0f	// a cell's center above the floor it stands on (the fit box's bottom)
#define NO_FLOOR		-1e30f
#define PAD_FLOOR		1e30f	// in or above a jump pad: climb as high as it goes
#define PLENTY_ROCKETS	5		// a map handing out this many rockets allows rocket jumps anywhere

// the box used to decide whether a player fits in a cell: crouched, so low passages count
static const vec3_t	fitMins = { -15, -15, -24 };
static const vec3_t	fitMaxs = { 15, 15, 16 };

static struct {
	int		built;
	float	cell;
	vec3_t	origin;			// world position of the corner of cell (0, 0, 0)
	int		size[3];
	// per target, per cell: distance to the target zone, or -1 if blocked or unreachable
	float	*dist[GS_ROUTE_MAX_TARGETS];
	int		numTargets;
	int		targetEntity[GS_ROUTE_MAX_TARGETS];	// checkpoint entity per target, -1 for start/finish
	unsigned char *air;		// per cell: no floor within SUPPORT_HEIGHT below
	float	*floorZ;		// per cell: the floor below it a jump can start from, NO_FLOOR or PAD_FLOOR
} route;

static int CellIndex( int x, int y, int z ) {
	return ( z * route.size[1] + y ) * route.size[0] + x;
}

static void CellCoords( int index, int c[3] ) {
	c[0] = index % route.size[0];
	c[1] = ( index / route.size[0] ) % route.size[1];
	c[2] = index / ( route.size[0] * route.size[1] );
}

static int InGrid( int x, int y, int z ) {
	return x >= 0 && y >= 0 && z >= 0 && x < route.size[0] && y < route.size[1] && z < route.size[2];
}

static void CellCenter( int index, vec3_t out ) {
	int c[3];
	CellCoords( index, c );
	for ( int i = 0; i < 3; i++ ) {
		out[i] = route.origin[i] + ( c[i] + 0.5f ) * route.cell;
	}
}

// The open cell (dist >= 0) nearest to pos, within `radius` cells of the one containing
// it, or -1. Used where a point sits in or right next to a wall at grid resolution: a
// teleport destination just above the floor, or a player brushing a wall.
static int NearestOpenCell( const float *dist, const vec3_t pos, int radius ) {
	int c[3], best = -1, inside = 1;
	float bestDist = 1e30f;
	vec3_t center;
	for ( int i = 0; i < 3; i++ ) {
		float g = ( pos[i] - route.origin[i] ) / route.cell;
		c[i] = (int)floorf( g );
		inside &= g - c[i] > 0.01f && g - c[i] < 0.99f;
	}
	// The usual case: pos is well inside an open cell (not within a rounding error of a
	// face, where a neighbour's center can be as near), whose center is then nearer than
	// any other cell's: the search below would find the same cell.
	if ( inside && InGrid( c[0], c[1], c[2] ) && dist[ CellIndex( c[0], c[1], c[2] ) ] >= 0 ) {
		return CellIndex( c[0], c[1], c[2] );
	}
	for ( int z = c[2] - radius; z <= c[2] + radius; z++ ) {
		for ( int y = c[1] - radius; y <= c[1] + radius; y++ ) {
			for ( int x = c[0] - radius; x <= c[0] + radius; x++ ) {
				if ( !InGrid( x, y, z ) || dist[ CellIndex( x, y, z ) ] < 0 ) {
					continue;
				}
				int n = CellIndex( x, y, z );
				CellCenter( n, center );
				float d = Distance( pos, center );
				if ( d < bestDist ) {
					bestDist = d;
					best = n;
				}
			}
		}
	}
	return best;
}

// ---------------------------------------------------------------------------
// the triggers that shape the route

// Does the box at `center` touch any of the listed trigger entities?
static int TouchesAny( const vec3_t center, const int *ents, int numEnts ) {
	vec3_t mins, maxs;
	VectorAdd( center, fitMins, mins );
	VectorAdd( center, fitMaxs, maxs );
	for ( int i = 0; i < numEnts; i++ ) {
		const gsEnt_t *e = &gs_world.ents[ ents[i] ];
		if ( mins[0] > e->absmax[0] || maxs[0] < e->absmin[0] || mins[1] > e->absmax[1]
			|| maxs[1] < e->absmin[1] || mins[2] > e->absmax[2] || maxs[2] < e->absmin[2] ) {
			continue;
		}
		if ( GS_EntityContact( mins, maxs, e ) ) {
			return 1;
		}
	}
	return 0;
}

// Does this trigger fire a target of the given type?
static int TriggerFires( const gsEnt_t *trigger, gsEntType_t type ) {
	for ( int i = 0; i < trigger->numTargets; i++ ) {
		if ( gs_world.ents[ trigger->targets[i] ].type == type ) {
			return 1;
		}
	}
	return 0;
}

// The destination entity of a teleport trigger (or trigger -> target_teleporter), or NULL.
static const gsEnt_t *TeleportDest( const gsEnt_t *trigger ) {
	if ( trigger->type == GE_TRIGGER_TELEPORT && trigger->numTargets && !( trigger->spawnflags & 1 ) ) {
		return &gs_world.ents[ trigger->targets[0] ];
	}
	if ( trigger->type == GE_TRIGGER_MULTIPLE ) {
		for ( int i = 0; i < trigger->numTargets; i++ ) {
			const gsEnt_t *t = &gs_world.ents[ trigger->targets[i] ];
			if ( t->type == GE_TARGET_TELEPORTER && t->numTargets ) {
				return &gs_world.ents[ t->targets[0] ];
			}
		}
	}
	return NULL;
}

typedef struct {
	int		goals[GS_ROUTE_MAX_TARGETS][GS_MAX_ENTITIES];	// the triggers of each target zone
	int		numGoals[GS_ROUTE_MAX_TARGETS];
	int		deadly[GS_MAX_ENTITIES];
	int		numDeadly;
	int		pads[GS_MAX_ENTITIES];		// jump pads: trigger_push, or a trigger firing a target_push
	int		numPads;
	int		teleports[GS_MAX_ENTITIES];
	int		teleportDestCell[GS_MAX_ENTITIES];		// the open cell each teleport lands in
	int		numTeleports;
	unsigned char *isTeleportDest;					// per cell: some teleport lands here
} triggers_t;

static void FindTriggers( triggers_t *t ) {
	memset( t, 0, sizeof( *t ) );
	for ( int i = 0; i < gs_world.numEnts; i++ ) {
		const gsEnt_t *e = &gs_world.ents[i];
		if ( !e->model || !( e->contents & CONTENTS_TRIGGER ) ) {
			continue;
		}
		if ( e->type == GE_TRIGGER_MULTIPLE && TriggerFires( e, GE_TARGET_START_TIMER ) ) {
			t->goals[GS_ROUTE_START][ t->numGoals[GS_ROUTE_START]++ ] = i;
		}
		if ( e->type == GE_TRIGGER_MULTIPLE && TriggerFires( e, GE_TARGET_STOP_TIMER ) ) {
			t->goals[GS_ROUTE_FINISH][ t->numGoals[GS_ROUTE_FINISH]++ ] = i;
		}
		// each checkpoint is its own target: the triggers that fire it
		for ( int k = 0; e->type == GE_TRIGGER_MULTIPLE && k < e->numTargets; k++ ) {
			const gsEnt_t *cp = &gs_world.ents[ e->targets[k] ];
			if ( cp->type == GE_TARGET_CHECKPOINT ) {
				int target = GS_ROUTE_CHECKPOINT0 + cp->slot;
				t->goals[target][ t->numGoals[target]++ ] = i;
			}
		}
		if ( e->type == GE_TRIGGER_HURT || ( e->type == GE_TRIGGER_MULTIPLE && TriggerFires( e, GE_TARGET_KILL ) ) ) {
			t->deadly[ t->numDeadly++ ] = i;
		}
		if ( TeleportDest( e ) ) {
			t->teleports[ t->numTeleports++ ] = i;
		}
		if ( e->type == GE_TRIGGER_PUSH || ( e->type == GE_TRIGGER_MULTIPLE && TriggerFires( e, GE_TARGET_PUSH ) ) ) {
			t->pads[ t->numPads++ ] = i;
		}
	}
}

// Is the box at `center` in a jump pad's trigger, or straight above one (where the
// pad launches the player)?
static int InOrAbovePad( const vec3_t center, const triggers_t *t ) {
	for ( int k = 0; k < t->numPads; k++ ) {
		const gsEnt_t *pad = &gs_world.ents[ t->pads[k] ];
		if ( center[0] + fitMaxs[0] > pad->absmin[0] && center[0] + fitMins[0] < pad->absmax[0]
			&& center[1] + fitMaxs[1] > pad->absmin[1] && center[1] + fitMins[1] < pad->absmax[1]
			&& center[2] + fitMaxs[2] > pad->absmin[2] ) {
			return 1;
		}
	}
	return 0;
}

// How many rockets the map hands out: rocket launchers and rocket ammo, on the floor
// or given by a target_give.
static int RocketSupply( void ) {
	int rockets = 0;
	for ( int i = 0; i < gs_world.numEnts; i++ ) {
		const gsEnt_t *e = &gs_world.ents[i];
		if ( e->type == GE_ITEM && e->item->giTag == WP_ROCKET_LAUNCHER
			&& ( e->item->giType == IT_WEAPON || e->item->giType == IT_AMMO ) ) {
			rockets += e->quantity;
		}
	}
	return rockets;
}

// Does the spawn point reach the target whose distances are in dist?
static int SpawnReaches( const float *dist ) {
	if ( gs_world.spawnEnt < 0 ) {
		return 1;
	}
	vec3_t spawn;
	VectorCopy( gs_world.ents[ gs_world.spawnEnt ].origin, spawn );
	spawn[2] += 9;		// SpawnPoint lifts the player above the entity's origin
	int cell = NearestOpenCell( dist, spawn, 2 );
	return cell >= 0;
}

// ---------------------------------------------------------------------------
// Dijkstra, with a binary heap of (distance, cell). Cells can be pushed more than
// once; stale entries are skipped when popped.

typedef struct {
	float	dist;
	int		cell;
} heapItem_t;

static struct {
	heapItem_t	*items;
	int			count, capacity;
} heap;

static void HeapPush( float dist, int cell ) {
	if ( heap.count == heap.capacity ) {
		heap.capacity = heap.capacity ? heap.capacity * 2 : 1 << 16;
		heap.items = realloc( heap.items, heap.capacity * sizeof( heapItem_t ) );
	}
	int i = heap.count++;
	while ( i > 0 && heap.items[ ( i - 1 ) / 2 ].dist > dist ) {
		heap.items[i] = heap.items[ ( i - 1 ) / 2 ];
		i = ( i - 1 ) / 2;
	}
	heap.items[i] = (heapItem_t){ dist, cell };
}

static heapItem_t HeapPop( void ) {
	heapItem_t top = heap.items[0];
	heapItem_t last = heap.items[ --heap.count ];
	int i = 0;
	while ( 1 ) {
		int child = 2 * i + 1;
		if ( child >= heap.count ) {
			break;
		}
		if ( child + 1 < heap.count && heap.items[ child + 1 ].dist < heap.items[child].dist ) {
			child++;
		}
		if ( heap.items[child].dist >= last.dist ) {
			break;
		}
		heap.items[i] = heap.items[child];
		i = child;
	}
	heap.items[i] = last;
	return top;
}

static void Relax( float *dist, int cell, float d ) {
	if ( dist[cell] >= 0 && d < dist[cell] ) {
		dist[cell] = d;
		HeapPush( d, cell );
	}
}

// Fill dist (open cells UNREACHED, blocked -1) with each open cell's distance to the
// goal triggers. Cells that can't reach them end up -1.
static void Flood( float *dist, const triggers_t *t, const int *goals, int numGoals ) {
	int numCells = route.size[0] * route.size[1] * route.size[2];
	vec3_t center;

	heap.count = 0;
	for ( int c = 0; c < numCells; c++ ) {
		if ( dist[c] >= 0 ) {
			CellCenter( c, center );
			if ( TouchesAny( center, goals, numGoals ) ) {
				dist[c] = 0;
				HeapPush( 0, c );
			}
		}
	}

	while ( heap.count ) {
		heapItem_t it = HeapPop();
		if ( it.dist > dist[ it.cell ] ) {
			continue;	// stale entry
		}
		int c[3];
		CellCoords( it.cell, c );

		// The 26 neighbours. We search backwards from the goal, so the player moves
		// from the neighbour to this cell: it climbs when dz < 0, and the sideways part
		// of the step is through air if the neighbour has no floor below. A climb is
		// only possible to here if a jump from the neighbour's floor reaches.
		CellCenter( it.cell, center );
		for ( int dz = -1; dz <= 1; dz++ ) {
			float up = dz < 0 ? CLIMB_COST : 1;
			for ( int dy = -1; dy <= 1; dy++ ) {
				for ( int dx = -1; dx <= 1; dx++ ) {
					if ( !( dx || dy || dz ) || !InGrid( c[0] + dx, c[1] + dy, c[2] + dz ) ) {
						continue;
					}
					int n = CellIndex( c[0] + dx, c[1] + dy, c[2] + dz );
					if ( dz < 0 && center[2] > route.floorZ[n] + STAND_HEIGHT + JUMP_REACH ) {
						continue;
					}
					float side = sqrtf( (float)( dx * dx + dy * dy ) ) * ( route.air[n] ? AIR_COST : 1 );
					float step = sqrtf( side * side + ( dz * up ) * ( dz * up ) );
					Relax( dist, n, it.dist + route.cell * step );
				}
			}
		}

		// Reaching a teleport's destination also reaches every cell of its trigger,
		// at the same distance: stepping in is a free jump to here.
		for ( int k = 0; t->isTeleportDest[ it.cell ] && k < t->numTeleports; k++ ) {
			if ( t->teleportDestCell[k] != it.cell ) {
				continue;
			}
			const gsEnt_t *trig = &gs_world.ents[ t->teleports[k] ];
			int lo[3], hi[3];
			for ( int i = 0; i < 3; i++ ) {
				lo[i] = (int)floorf( ( trig->absmin[i] - fitMaxs[i] - route.origin[i] ) / route.cell );
				hi[i] = (int)floorf( ( trig->absmax[i] - fitMins[i] - route.origin[i] ) / route.cell );
			}
			for ( int z = lo[2]; z <= hi[2]; z++ ) {
				for ( int y = lo[1]; y <= hi[1]; y++ ) {
					for ( int x = lo[0]; x <= hi[0]; x++ ) {
						if ( InGrid( x, y, z ) ) {
							int n = CellIndex( x, y, z );
							CellCenter( n, center );
							if ( TouchesAny( center, &t->teleports[k], 1 ) ) {
								Relax( dist, n, it.dist );
							}
						}
					}
				}
			}
		}
	}

	for ( int c = 0; c < numCells; c++ ) {
		if ( dist[c] == UNREACHED ) {
			dist[c] = -1;
		}
	}
}

// ---------------------------------------------------------------------------

int gs_route_build( float cell ) {
	static triggers_t t;
	vec3_t mins, maxs, center;

	route.built = 0;
	if ( !gs_world.loaded ) {
		GS_SetError( "gs_route_build: no map loaded" );
		return -1;
	}
	FindTriggers( &t );
	if ( !t.numGoals[GS_ROUTE_START] || !t.numGoals[GS_ROUTE_FINISH] ) {
		GS_SetError( "gs_route_build: %s needs triggers that fire a target_startTimer and a target_stopTimer",
					 gs_world.mapname );
		return -1;
	}

	// the grid covers the world bounds
	route.cell = cell > 0 ? cell : DEFAULT_CELL;
	CM_ModelBounds( 0, mins, maxs );
	for ( int i = 0; i < 3; i++ ) {
		route.origin[i] = mins[i];
		route.size[i] = (int)ceilf( ( maxs[i] - mins[i] ) / route.cell ) + 1;
	}
	int numCells = route.size[0] * route.size[1] * route.size[2];

	// targets: start, finish, then one per checkpoint
	route.numTargets = GS_ROUTE_CHECKPOINT0 + gs_world.numCheckpoints;
	for ( int k = 0; k < GS_ROUTE_MAX_TARGETS; k++ ) {
		route.targetEntity[k] = -1;
	}
	for ( int i = 0; i < gs_world.numEnts; i++ ) {
		if ( gs_world.ents[i].type == GE_TARGET_CHECKPOINT ) {
			route.targetEntity[ GS_ROUTE_CHECKPOINT0 + gs_world.ents[i].slot ] = i;
		}
	}
	for ( int k = 0; k < GS_ROUTE_MAX_TARGETS; k++ ) {
		free( route.dist[k] );
		route.dist[k] = k < route.numTargets ? malloc( numCells * sizeof( float ) ) : NULL;
	}
	free( route.air );
	route.air = calloc( numCells, 1 );
	free( route.floorZ );
	route.floorZ = malloc( numCells * sizeof( float ) );

	// open cells (the player fits, no lava, no kill zone) start UNREACHED, blocked ones -1
	int rocketJumps = RocketSupply() >= PLENTY_ROCKETS;
	float *open = malloc( numCells * sizeof( float ) );
	for ( int c = 0; c < numCells; c++ ) {
		trace_t tr;
		CellCenter( c, center );
		GS_Trace( &tr, center, fitMins, fitMaxs, center, GS_PLAYER_NUM, MASK_PLAYERSOLID );
		int isOpen = !tr.startsolid
			&& !( GS_PointContents( center, GS_PLAYER_NUM ) & ( CONTENTS_LAVA | CONTENTS_SLIME ) )
			&& !TouchesAny( center, t.deadly, t.numDeadly );
		open[c] = isOpen ? UNREACHED : -1;
		if ( isOpen ) {
			// air: no floor close below, or the floor is inside a kill zone (standing
			// on it would kill, so it's no use for getting anywhere)
			vec3_t below, standing;
			VectorCopy( center, below );
			below[2] -= SUPPORT_HEIGHT;
			GS_Trace( &tr, center, NULL, NULL, below, GS_PLAYER_NUM, MASK_PLAYERSOLID );
			VectorCopy( tr.endpos, standing );
			standing[2] += 24;		// a standing player's origin on that floor
			route.air[c] = tr.fraction == 1.0f || TouchesAny( standing, t.deadly, t.numDeadly );
			route.floorZ[c] = route.air[c] ? NO_FLOOR : tr.endpos[2];
			if ( rocketJumps || InOrAbovePad( center, &t ) ) {
				route.floorZ[c] = PAD_FLOOR;
			}
		}
	}

	// the open cell each teleport lands in (TeleportPlayer lifts the player by one unit)
	t.isTeleportDest = calloc( numCells, 1 );
	for ( int k = 0; k < t.numTeleports; k++ ) {
		vec3_t landing;
		VectorCopy( TeleportDest( &gs_world.ents[ t.teleports[k] ] )->origin, landing );
		landing[2] += 1;
		t.teleportDestCell[k] = NearestOpenCell( open, landing, 3 );
		if ( t.teleportDestCell[k] >= 0 ) {
			t.isTeleportDest[ t.teleportDestCell[k] ] = 1;
		}
	}

	// Flood from every target. If the jump limit leaves the spawn cut off from a target,
	// the map must need climbs it doesn't know about (jumps off slopes, say): flood
	// again without it.
	int limited = !rocketJumps;
	for ( ;; ) {
		int connected = 1;
		for ( int k = 0; k < route.numTargets; k++ ) {
			memcpy( route.dist[k], open, numCells * sizeof( float ) );
			Flood( route.dist[k], &t, t.goals[k], t.numGoals[k] );
			connected &= SpawnReaches( route.dist[k] );
		}
		if ( connected || !limited ) {
			break;
		}
		Com_Printf( "route: the spawn can't reach every zone within jump height; climbing without a limit\n" );
		for ( int c = 0; c < numCells; c++ ) {
			route.floorZ[c] = PAD_FLOOR;
		}
		limited = 0;
	}

	free( t.isTeleportDest );
	free( open );

	int numReached[2] = { 0 };
	for ( int c = 0; c < numCells; c++ ) {
		numReached[0] += route.dist[GS_ROUTE_START][c] >= 0;
		numReached[1] += route.dist[GS_ROUTE_FINISH][c] >= 0;
	}
	Com_Printf( "route: %ix%ix%i cells of %.0f units; %i reach the start zone, %i the finish; "
				"%i checkpoints (%i deadly / %i teleport triggers)\n", route.size[0], route.size[1],
				route.size[2], route.cell, numReached[0], numReached[1], gs_world.numCheckpoints,
				t.numDeadly, t.numTeleports );
	route.built = 1;
	return 0;
}

int gs_route_num_targets( void ) {
	return route.built ? route.numTargets : 0;
}

int gs_route_target_entity( int target ) {
	return route.built && target >= 0 && target < route.numTargets ? route.targetEntity[target] : -1;
}

// ---------------------------------------------------------------------------
// lookups

float gs_route_distance( int target, const float pos[3] ) {
	if ( !route.built || target < 0 || target >= route.numTargets ) {
		return -1;
	}
	const float *dist = route.dist[target];

	// Blend the 8 cell centers around pos (trilinear), skipping blocked ones, so the
	// distance changes smoothly as the player moves.
	vec3_t f;
	int c0[3];
	for ( int i = 0; i < 3; i++ ) {
		float g = ( pos[i] - route.origin[i] ) / route.cell - 0.5f;
		c0[i] = (int)floorf( g );
		f[i] = g - c0[i];
	}
	float sum = 0, weight = 0;
	for ( int corner = 0; corner < 8; corner++ ) {
		int dx = corner & 1, dy = ( corner >> 1 ) & 1, dz = corner >> 2;
		if ( !InGrid( c0[0] + dx, c0[1] + dy, c0[2] + dz ) ) {
			continue;
		}
		float d = dist[ CellIndex( c0[0] + dx, c0[1] + dy, c0[2] + dz ) ];
		if ( d < 0 ) {
			continue;
		}
		float w = ( dx ? f[0] : 1 - f[0] ) * ( dy ? f[1] : 1 - f[1] ) * ( dz ? f[2] : 1 - f[2] );
		sum += w * d;
		weight += w;
	}
	if ( weight > 0 ) {
		return sum / weight;
	}

	// pos is in or right next to a wall at grid resolution: use the nearest open cell
	int near = NearestOpenCell( dist, pos, 2 );
	if ( near < 0 ) {
		return -1;
	}
	vec3_t center;
	CellCenter( near, center );
	return dist[near] + Distance( pos, center );
}

int gs_route_ahead( int target, const float pos[3], float ahead, float out[3] ) {
	return GS_RouteAheadMany( target, pos, &ahead, 1, (vec3_t *)out );
}

// gs_route_ahead for several distances (in increasing order) from one walk, which is
// cheaper than walking from pos for each: out[i] is the point ahead[i] further along.
int GS_RouteAheadMany( int target, const vec3_t pos, const float *ahead, int count, vec3_t *out ) {
	if ( !route.built || target < 0 || target >= route.numTargets ) {
		return -1;
	}
	const float *dist = route.dist[target];

	// start from the open cell nearest to pos, then walk downhill cell by cell
	int cell = NearestOpenCell( dist, pos, 2 );
	if ( cell < 0 ) {
		return -1;
	}
	float walked = 0;
	int done = 0;
	while ( done < count ) {
		while ( done < count && walked >= ahead[done] ) {
			CellCenter( cell, out[done++] );
		}
		if ( done == count || dist[cell] <= 0 ) {
			break;
		}
		int c[3], next = -1;
		float nextDist = dist[cell];
		CellCoords( cell, c );
		for ( int dz = -1; dz <= 1; dz++ ) {
			for ( int dy = -1; dy <= 1; dy++ ) {
				for ( int dx = -1; dx <= 1; dx++ ) {
					if ( !InGrid( c[0] + dx, c[1] + dy, c[2] + dz ) ) {
						continue;
					}
					int n = CellIndex( c[0] + dx, c[1] + dy, c[2] + dz );
					if ( dist[n] >= 0 && dist[n] < nextDist ) {
						nextDist = dist[n];
						next = n;
					}
				}
			}
		}
		if ( next < 0 ) {
			break;	// a teleport trigger: the route continues at the destination
		}
		walked += dist[cell] - nextDist;
		cell = next;
	}
	while ( done < count ) {
		CellCenter( cell, out[done++] );	// the route ended (the target, or a teleport) first
	}
	return 0;
}
