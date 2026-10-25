// route.c -- the route: how far every point of the map is from the finish.
//
// gs_route_build covers the map with a grid of cubes ("cells"), marks the cells a
// (crouching) player fits in, and then works out, for every open cell, the length of
// the shortest path to the finish zone through open cells (Dijkstra's algorithm).
// Teleporters count as a free jump from the trigger to the destination.
//
// Gravity is ignored: the route floats across gaps and up shafts. That's what we want
// for a reward. It says which way is forward and how far is left, and leaves *how* to
// get there (jumping, strafing) to the player.

#include "sim_local.h"

#define DEFAULT_CELL	16.0f

// the box used to decide whether a player fits in a cell: crouched, so low passages count
static const vec3_t	fitMins = { -15, -15, -24 };
static const vec3_t	fitMaxs = { 15, 15, 16 };

static struct {
	int		built;
	float	cell;
	vec3_t	origin;			// world position of the corner of cell (0, 0, 0)
	int		size[3];
	float	*dist;			// per cell: distance to the finish, or -1 if blocked or unreachable
} route;

static int CellIndex( int x, int y, int z ) {
	return ( z * route.size[1] + y ) * route.size[0] + x;
}

static void CellCenter( int index, vec3_t out ) {
	int x = index % route.size[0];
	int y = ( index / route.size[0] ) % route.size[1];
	int z = index / ( route.size[0] * route.size[1] );
	out[0] = route.origin[0] + ( x + 0.5f ) * route.cell;
	out[1] = route.origin[1] + ( y + 0.5f ) * route.cell;
	out[2] = route.origin[2] + ( z + 0.5f ) * route.cell;
}

// The open cell nearest to pos, within `radius` cells of the one containing it, or -1.
// Used where a point sits in or right next to a wall at grid resolution: a teleport
// destination just above the floor, or a player brushing a wall.
static int NearestOpenCell( const vec3_t pos, int radius ) {
	int c[3], best = -1;
	float bestDist = 1e30f;
	vec3_t center;
	for ( int i = 0; i < 3; i++ ) {
		c[i] = (int)floorf( ( pos[i] - route.origin[i] ) / route.cell );
	}
	for ( int z = c[2] - radius; z <= c[2] + radius; z++ ) {
		for ( int y = c[1] - radius; y <= c[1] + radius; y++ ) {
			for ( int x = c[0] - radius; x <= c[0] + radius; x++ ) {
				if ( x < 0 || y < 0 || z < 0 || x >= route.size[0] || y >= route.size[1] || z >= route.size[2] ) {
					continue;
				}
				int n = CellIndex( x, y, z );
				if ( route.dist[n] < 0 ) {
					continue;
				}
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
// which cells are open

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

// ---------------------------------------------------------------------------

int gs_route_build( float cell ) {
	int finish[GS_MAX_ENTITIES], numFinish = 0;
	int deadly[GS_MAX_ENTITIES], numDeadly = 0;
	int teleports[GS_MAX_ENTITIES], numTeleports = 0;
	int teleportDestCell[GS_MAX_ENTITIES];
	vec3_t mins, maxs, center;

	route.built = 0;
	if ( !gs_world.loaded ) {
		GS_SetError( "gs_route_build: no map loaded" );
		return -1;
	}

	// the triggers that matter: finish, death, teleport
	for ( int i = 0; i < gs_world.numEnts; i++ ) {
		const gsEnt_t *e = &gs_world.ents[i];
		if ( !e->model || !( e->contents & CONTENTS_TRIGGER ) ) {
			continue;
		}
		if ( e->type == GE_TRIGGER_MULTIPLE && TriggerFires( e, GE_TARGET_STOP_TIMER ) ) {
			finish[numFinish++] = i;
		}
		if ( e->type == GE_TRIGGER_HURT || ( e->type == GE_TRIGGER_MULTIPLE && TriggerFires( e, GE_TARGET_KILL ) ) ) {
			deadly[numDeadly++] = i;
		}
		if ( TeleportDest( e ) ) {
			teleports[numTeleports++] = i;
		}
	}
	if ( !numFinish ) {
		GS_SetError( "gs_route_build: %s has no finish (no trigger fires a target_stopTimer)", gs_world.mapname );
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
	free( route.dist );
	route.dist = malloc( numCells * sizeof( float ) );

	// open cells start at "infinity", blocked ones at -1
	unsigned char *isFinish = calloc( numCells, 1 );
	unsigned char *isTeleDest = calloc( numCells, 1 );
	for ( int c = 0; c < numCells; c++ ) {
		trace_t tr;
		CellCenter( c, center );
		GS_Trace( &tr, center, fitMins, fitMaxs, center, GS_PLAYER_NUM, MASK_PLAYERSOLID );
		int open = !tr.startsolid
			&& !( GS_PointContents( center, GS_PLAYER_NUM ) & ( CONTENTS_LAVA | CONTENTS_SLIME ) )
			&& !TouchesAny( center, deadly, numDeadly );
		route.dist[c] = open ? 1e30f : -1;
		isFinish[c] = open && TouchesAny( center, finish, numFinish );
	}

	// teleport destinations: the open cell the player lands in
	for ( int t = 0; t < numTeleports; t++ ) {
		const gsEnt_t *dest = TeleportDest( &gs_world.ents[ teleports[t] ] );
		vec3_t landing;
		VectorCopy( dest->origin, landing );
		landing[2] += 1;	// TeleportPlayer lifts the player by one unit
		teleportDestCell[t] = NearestOpenCell( landing, 3 );
		if ( teleportDestCell[t] >= 0 ) {
			isTeleDest[ teleportDestCell[t] ] = 1;
		}
	}

	// Dijkstra from every finish cell outwards
	heap.count = 0;
	for ( int c = 0; c < numCells; c++ ) {
		if ( isFinish[c] ) {
			route.dist[c] = 0;
			HeapPush( 0, c );
		}
	}
	while ( heap.count ) {
		heapItem_t it = HeapPop();
		if ( it.dist > route.dist[ it.cell ] ) {
			continue;	// stale entry
		}
		int x = it.cell % route.size[0];
		int y = ( it.cell / route.size[0] ) % route.size[1];
		int z = it.cell / ( route.size[0] * route.size[1] );

		// the 26 neighbours, one cell, sqrt(2) or sqrt(3) cells away
		for ( int dz = -1; dz <= 1; dz++ ) {
			for ( int dy = -1; dy <= 1; dy++ ) {
				for ( int dx = -1; dx <= 1; dx++ ) {
					int nx = x + dx, ny = y + dy, nz = z + dz;
					if ( ( !dx && !dy && !dz ) || nx < 0 || ny < 0 || nz < 0
						|| nx >= route.size[0] || ny >= route.size[1] || nz >= route.size[2] ) {
						continue;
					}
					int n = CellIndex( nx, ny, nz );
					float d = it.dist + route.cell * sqrtf( (float)( dx * dx + dy * dy + dz * dz ) );
					if ( route.dist[n] >= 0 && d < route.dist[n] ) {
						route.dist[n] = d;
						HeapPush( d, n );
					}
				}
			}
		}

		// Reaching a teleport destination also reaches every cell of the teleport's
		// trigger, at the same distance: stepping in is a free jump to here.
		if ( isTeleDest[ it.cell ] ) {
			for ( int t = 0; t < numTeleports; t++ ) {
				if ( teleportDestCell[t] != it.cell ) {
					continue;
				}
				const gsEnt_t *trig = &gs_world.ents[ teleports[t] ];
				int lo[3], hi[3];
				for ( int i = 0; i < 3; i++ ) {
					lo[i] = (int)floorf( ( trig->absmin[i] - fitMaxs[i] - route.origin[i] ) / route.cell );
					hi[i] = (int)floorf( ( trig->absmax[i] - fitMins[i] - route.origin[i] ) / route.cell );
					lo[i] = lo[i] < 0 ? 0 : lo[i];
					hi[i] = hi[i] >= route.size[i] ? route.size[i] - 1 : hi[i];
				}
				for ( int cz = lo[2]; cz <= hi[2]; cz++ ) {
					for ( int cy = lo[1]; cy <= hi[1]; cy++ ) {
						for ( int cx = lo[0]; cx <= hi[0]; cx++ ) {
							int n = CellIndex( cx, cy, cz );
							CellCenter( n, center );
							if ( route.dist[n] >= 0 && it.dist < route.dist[n] && TouchesAny( center, &teleports[t], 1 ) ) {
								route.dist[n] = it.dist;
								HeapPush( it.dist, n );
							}
						}
					}
				}
			}
		}
	}

	// open cells the finish can't be reached from
	int numOpen = 0, numReached = 0, numFinishCells = 0;
	for ( int c = 0; c < numCells; c++ ) {
		numOpen += route.dist[c] >= 0;
		numFinishCells += isFinish[c];
		if ( route.dist[c] > 1e29f ) {
			route.dist[c] = -1;
		}
		numReached += route.dist[c] >= 0;
	}
	Com_Printf( "route: %ix%ix%i cells of %.0f units, %i open, %i in the finish, %i reach it "
				"(%i finish / %i deadly / %i teleport triggers)\n", route.size[0], route.size[1], route.size[2],
				route.cell, numOpen, numFinishCells, numReached, numFinish, numDeadly, numTeleports );
	free( isFinish );
	free( isTeleDest );
	route.built = 1;
	return 0;
}

// ---------------------------------------------------------------------------
// lookups

float gs_route_distance( const float pos[3] ) {
	if ( !route.built ) {
		return -1;
	}
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
		int c[3] = { c0[0] + ( corner & 1 ), c0[1] + ( ( corner >> 1 ) & 1 ), c0[2] + ( corner >> 2 ) };
		if ( c[0] < 0 || c[1] < 0 || c[2] < 0 || c[0] >= route.size[0] || c[1] >= route.size[1] || c[2] >= route.size[2] ) {
			continue;
		}
		float d = route.dist[ CellIndex( c[0], c[1], c[2] ) ];
		if ( d < 0 ) {
			continue;
		}
		float w = ( corner & 1 ? f[0] : 1 - f[0] ) * ( ( corner >> 1 ) & 1 ? f[1] : 1 - f[1] )
				* ( corner >> 2 ? f[2] : 1 - f[2] );
		sum += w * d;
		weight += w;
	}
	if ( weight > 0 ) {
		return sum / weight;
	}

	// pos is in or right next to a wall at grid resolution: use the nearest open cell
	int near = NearestOpenCell( pos, 1 );
	if ( near < 0 ) {
		return -1;
	}
	vec3_t center;
	CellCenter( near, center );
	return route.dist[near] + Distance( pos, center );
}

int gs_route_ahead( const float pos[3], float ahead, float out[3] ) {
	// start from the open cell nearest to pos, then walk downhill cell by cell
	if ( !route.built ) {
		return -1;
	}
	int cell = NearestOpenCell( pos, 1 );
	if ( cell < 0 ) {
		return -1;
	}

	float walked = 0;
	while ( walked < ahead && route.dist[cell] > 0 ) {
		int x = cell % route.size[0], y = ( cell / route.size[0] ) % route.size[1];
		int z = cell / ( route.size[0] * route.size[1] );
		int next = -1;
		float nextDist = route.dist[cell];
		for ( int dz = -1; dz <= 1; dz++ ) {
			for ( int dy = -1; dy <= 1; dy++ ) {
				for ( int dx = -1; dx <= 1; dx++ ) {
					int nx = x + dx, ny = y + dy, nz = z + dz;
					if ( nx < 0 || ny < 0 || nz < 0 || nx >= route.size[0] || ny >= route.size[1] || nz >= route.size[2] ) {
						continue;
					}
					int n = CellIndex( nx, ny, nz );
					if ( route.dist[n] >= 0 && route.dist[n] < nextDist ) {
						nextDist = route.dist[n];
						next = n;
					}
				}
			}
		}
		if ( next < 0 ) {
			break;	// a teleport trigger: the route continues at the destination
		}
		walked += route.dist[cell] - nextDist;
		cell = next;
	}
	CellCenter( cell, out );
	return 0;
}
