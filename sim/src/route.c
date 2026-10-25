// route.c -- the route: how far every point of the map is from the start zone and
// from the finish.
//
// gs_route_build covers the map with a grid of cubes ("cells"), marks the cells a
// (crouching) player fits in, and then works out, for every open cell, the length of
// the shortest path through open cells to the start zone and to the finish zone
// (Dijkstra's algorithm, once per target). Teleporters count as a free jump from the
// trigger to the destination.
//
// A DeFRaG run has two legs: get to the start zone (which starts the timer), then to
// the finish (which stops it). Hence two targets.
//
// Gravity is only roughly accounted for, with two costs:
//  - climbing costs CLIMB_COST times as much as moving sideways or down, so the route
//    goes around tall walls rather than over them, and happily drops down holes;
//  - moving sideways through "air" (no floor within SUPPORT_HEIGHT below) costs
//    AIR_COST times as much, so the route jumps gaps when it has to, but takes the
//    long way around a gap nobody could jump.
// That's what we want for a reward. It says which way is forward and how far is left,
// and leaves *how* to get there (jumping, strafing) to the player.

#include "sim_local.h"

#define DEFAULT_CELL	16.0f
#define UNREACHED		1e30f
#define CLIMB_COST		8.0f	// each unit up counts as this many units of distance
#define AIR_COST		12.0f	// each unit sideways with no floor below, likewise
#define SUPPORT_HEIGHT	96.0f	// floor this close below a cell makes it "not air"

// the box used to decide whether a player fits in a cell: crouched, so low passages count
static const vec3_t	fitMins = { -15, -15, -24 };
static const vec3_t	fitMaxs = { 15, 15, 16 };

static struct {
	int		built;
	float	cell;
	vec3_t	origin;			// world position of the corner of cell (0, 0, 0)
	int		size[3];
	// per target, per cell: distance to the target zone, or -1 if blocked or unreachable
	float	*dist[GS_ROUTE_TARGETS];
	unsigned char *air;		// per cell: no floor within SUPPORT_HEIGHT below
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
	int c[3], best = -1;
	float bestDist = 1e30f;
	vec3_t center;
	for ( int i = 0; i < 3; i++ ) {
		c[i] = (int)floorf( ( pos[i] - route.origin[i] ) / route.cell );
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
	int		goals[GS_ROUTE_TARGETS][GS_MAX_ENTITIES];	// start-zone and finish triggers
	int		numGoals[GS_ROUTE_TARGETS];
	int		deadly[GS_MAX_ENTITIES];
	int		numDeadly;
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
		if ( e->type == GE_TRIGGER_HURT || ( e->type == GE_TRIGGER_MULTIPLE && TriggerFires( e, GE_TARGET_KILL ) ) ) {
			t->deadly[ t->numDeadly++ ] = i;
		}
		if ( TeleportDest( e ) ) {
			t->teleports[ t->numTeleports++ ] = i;
		}
	}
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
		// of the step is through air if the neighbour has no floor below.
		for ( int dz = -1; dz <= 1; dz++ ) {
			float up = dz < 0 ? CLIMB_COST : 1;
			for ( int dy = -1; dy <= 1; dy++ ) {
				for ( int dx = -1; dx <= 1; dx++ ) {
					if ( !( dx || dy || dz ) || !InGrid( c[0] + dx, c[1] + dy, c[2] + dz ) ) {
						continue;
					}
					int n = CellIndex( c[0] + dx, c[1] + dy, c[2] + dz );
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
	for ( int k = 0; k < GS_ROUTE_TARGETS; k++ ) {
		free( route.dist[k] );
		route.dist[k] = malloc( numCells * sizeof( float ) );
	}
	free( route.air );
	route.air = calloc( numCells, 1 );

	// open cells (the player fits, no lava, no kill zone) start UNREACHED, blocked ones -1
	float *open = route.dist[0];
	for ( int c = 0; c < numCells; c++ ) {
		trace_t tr;
		CellCenter( c, center );
		GS_Trace( &tr, center, fitMins, fitMaxs, center, GS_PLAYER_NUM, MASK_PLAYERSOLID );
		int isOpen = !tr.startsolid
			&& !( GS_PointContents( center, GS_PLAYER_NUM ) & ( CONTENTS_LAVA | CONTENTS_SLIME ) )
			&& !TouchesAny( center, t.deadly, t.numDeadly );
		open[c] = isOpen ? UNREACHED : -1;
		if ( isOpen ) {
			vec3_t below;
			VectorCopy( center, below );
			below[2] -= SUPPORT_HEIGHT;
			GS_Trace( &tr, center, NULL, NULL, below, GS_PLAYER_NUM, MASK_PLAYERSOLID );
			route.air[c] = tr.fraction == 1.0f;
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

	for ( int k = GS_ROUTE_TARGETS - 1; k >= 0; k-- ) {
		if ( k ) {
			memcpy( route.dist[k], open, numCells * sizeof( float ) );
		}
		Flood( route.dist[k], &t, t.goals[k], t.numGoals[k] );
	}

	free( t.isTeleportDest );

	int numReached[GS_ROUTE_TARGETS] = { 0 };
	for ( int c = 0; c < numCells; c++ ) {
		for ( int k = 0; k < GS_ROUTE_TARGETS; k++ ) {
			numReached[k] += route.dist[k][c] >= 0;
		}
	}
	Com_Printf( "route: %ix%ix%i cells of %.0f units; %i reach the start zone, %i the finish "
				"(%i deadly / %i teleport triggers)\n", route.size[0], route.size[1], route.size[2],
				route.cell, numReached[GS_ROUTE_START], numReached[GS_ROUTE_FINISH], t.numDeadly, t.numTeleports );
	route.built = 1;
	return 0;
}

// ---------------------------------------------------------------------------
// lookups

float gs_route_distance( int target, const float pos[3] ) {
	if ( !route.built || target < 0 || target >= GS_ROUTE_TARGETS ) {
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
	if ( !route.built || target < 0 || target >= GS_ROUTE_TARGETS ) {
		return -1;
	}
	const float *dist = route.dist[target];

	// start from the open cell nearest to pos, then walk downhill cell by cell
	int cell = NearestOpenCell( dist, pos, 2 );
	if ( cell < 0 ) {
		return -1;
	}
	float walked = 0;
	while ( walked < ahead && dist[cell] > 0 ) {
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
	CellCenter( cell, out );
	return 0;
}
