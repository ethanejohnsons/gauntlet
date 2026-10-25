// world.c -- world queries as the server answers them for the game (sv_world.c):
// entities in a box, entity contact, traces and point contents.

#include "sim_local.h"

int GS_EntitiesInBox( const vec3_t mins, const vec3_t maxs, int *list, int maxcount ) {
	int count = 0;
	for ( int i = 0; i < gs_world.numEnts && count < maxcount; i++ ) {
		const gsEnt_t *e = &gs_world.ents[i];
		if ( !e->contents ) {
			continue;	// not linked
		}
		if ( e->absmin[0] > maxs[0] || e->absmin[1] > maxs[1] || e->absmin[2] > maxs[2]
			|| e->absmax[0] < mins[0] || e->absmax[1] < mins[1] || e->absmax[2] < mins[2] ) {
			continue;
		}
		list[count++] = i;
	}
	return count;
}

// SV_EntityContact
int GS_EntityContact( const vec3_t mins, const vec3_t maxs, const gsEnt_t *ent ) {
	trace_t trace;
	CM_TransformedBoxTrace( &trace, vec3_origin, vec3_origin, mins, maxs, ent->model, -1,
							ent->origin, ent->angles, qfalse );
	return trace.startsolid;
}

// SV_Trace, with SV_ClipMoveToEntities for the solid brush entities
void GS_Trace( trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs,
			   const vec3_t end, int passEntityNum, int contentmask ) {
	trace_t		trace;
	vec3_t		boxmins, boxmaxs;
	int			touch[GS_MAX_ENTITIES];
	int			i, num;

	if ( !mins ) {
		mins = vec3_origin;
	}
	if ( !maxs ) {
		maxs = vec3_origin;
	}

	// clip to world
	CM_BoxTrace( results, start, end, mins, maxs, 0, contentmask, qfalse );
	results->entityNum = results->fraction != 1.0 ? ENTITYNUM_WORLD : ENTITYNUM_NONE;
	if ( results->fraction == 0 ) {
		return;		// blocked immediately by the world
	}

	for ( i=0 ; i<3 ; i++ ) {
		if ( end[i] > start[i] ) {
			boxmins[i] = start[i] + mins[i] - 1;
			boxmaxs[i] = end[i] + maxs[i] + 1;
		} else {
			boxmins[i] = end[i] + mins[i] - 1;
			boxmaxs[i] = start[i] + maxs[i] + 1;
		}
	}

	num = GS_EntitiesInBox( boxmins, boxmaxs, touch, GS_MAX_ENTITIES );
	for ( i=0 ; i<num ; i++ ) {
		const gsEnt_t *e = &gs_world.ents[ touch[i] ];
		if ( results->allsolid ) {
			return;
		}
		// items are trigger boxes; nothing that moves the player traces against them
		if ( !e->model || !( contentmask & e->contents ) ) {
			continue;
		}

		CM_TransformedBoxTrace( &trace, start, end, mins, maxs, e->model, contentmask,
								e->origin, e->angles, qfalse );

		if ( trace.allsolid ) {
			results->allsolid = qtrue;
			trace.entityNum = GS_FIRST_ENTITY + touch[i];
		} else if ( trace.startsolid ) {
			results->startsolid = qtrue;
			trace.entityNum = GS_FIRST_ENTITY + touch[i];
		}

		if ( trace.fraction < results->fraction ) {
			qboolean	oldStart;

			// make sure we keep a startsolid from a previous trace
			oldStart = results->startsolid;

			trace.entityNum = GS_FIRST_ENTITY + touch[i];
			*results = trace;
			results->startsolid |= oldStart;
		}
	}
}

// SV_PointContents
int GS_PointContents( const vec3_t p, int passEntityNum ) {
	int touch[GS_MAX_ENTITIES];
	int contents = CM_PointContents( p, 0 );
	int num = GS_EntitiesInBox( p, p, touch, GS_MAX_ENTITIES );
	for ( int i = 0; i < num; i++ ) {
		const gsEnt_t *e = &gs_world.ents[ touch[i] ];
		if ( !e->model ) {
			continue;	// items: see GS_Trace
		}
		contents |= CM_TransformedPointContents( p, e->model, e->origin, e->angles );
	}
	return contents;
}

float gs_trace( const float start[3], const float end[3], const float mins[3],
				const float maxs[3], float endpos[3] ) {
	trace_t tr;
	GS_Trace( &tr, start, mins, maxs, end, GS_PLAYER_NUM, MASK_PLAYERSOLID );
	if ( endpos ) {
		VectorCopy( tr.endpos, endpos );
	}
	return tr.fraction;
}

