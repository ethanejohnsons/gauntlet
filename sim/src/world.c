// world.c -- the map: loading a BSP, parsing its entities, and answering the world
// queries pmove and the game make (sv_world.c: traces, point contents, entities in
// a box, entity contact).

#include "sim_local.h"

gsWorld_t gs_world;
static char gs_error[1024];

const char *gs_last_error( void ) {
	return gs_error;
}

void GS_SetError( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( gs_error, sizeof( gs_error ), fmt, ap );
	va_end( ap );
}

void gs_set_verbose( int level ) {
	gs_verbose = level;
}

// ---------------------------------------------------------------------------
// entities

// What each entity class means for movement. Anything not listed counts as
// unsupported, so a map is only trusted when everything in it is understood.
static const struct {
	const char	*classname;
	gsEntType_t	type;
} classTypes[] = {
	{ "info_player_deathmatch",	GE_SPAWN },
	{ "info_player_start",		GE_SPAWN },
	{ "func_static",			GE_SOLID },
	{ "trigger_multiple",		GE_TRIGGER_MULTIPLE },
	{ "trigger_teleport",		GE_TRIGGER_TELEPORT },
	{ "trigger_push",			GE_TRIGGER_PUSH },
	{ "trigger_hurt",			GE_TRIGGER_HURT },
	{ "target_position",		GE_TARGET_POSITION },
	{ "misc_teleporter_dest",	GE_TARGET_POSITION },
	{ "info_notnull",			GE_TARGET_POSITION },
	{ "target_startTimer",		GE_TARGET_START_TIMER },
	{ "target_stopTimer",		GE_TARGET_STOP_TIMER },
	{ "target_checkpoint",		GE_TARGET_CHECKPOINT },
	{ "target_kill",			GE_TARGET_KILL },
	{ "target_teleporter",		GE_TARGET_TELEPORTER },
	{ "target_push",			GE_TARGET_PUSH },
	{ "item_haste",				GE_ITEM_POWERUP },
	{ "item_flight",			GE_ITEM_POWERUP },

	// target_give is left out on purpose: it can hand out a weapon a map needs (a
	// rocket jump over a gap), which the sim can't do yet, so it counts as unsupported

	// no effect on movement while the player never fires
	{ "worldspawn",				GE_IGNORED },
	{ "light",					GE_IGNORED },
	{ "misc_model",				GE_IGNORED },
	{ "info_null",				GE_IGNORED },
	{ "info_player_intermission", GE_IGNORED },
	{ "target_speaker",			GE_IGNORED },
	{ "target_print",			GE_IGNORED },
	{ "target_smallprint",		GE_IGNORED },
	{ "target_location",		GE_IGNORED },
	{ "target_init",			GE_IGNORED },
	{ "target_remove_powerups",	GE_IGNORED },
	{ "misc_portal_surface",	GE_IGNORED },
	{ "misc_portal_camera",		GE_IGNORED },
	{ "item_armor_shard",		GE_IGNORED },
	{ "item_armor_combat",		GE_IGNORED },
	{ "item_armor_body",		GE_IGNORED },
	{ "item_health",			GE_IGNORED },
	{ "item_health_small",		GE_IGNORED },
	{ "item_health_large",		GE_IGNORED },
	{ "item_health_mega",		GE_IGNORED },
	{ "item_enviro",			GE_IGNORED },
	{ "item_quad",				GE_IGNORED },
	{ "item_regen",				GE_IGNORED },
	{ "item_invis",				GE_IGNORED },
};

static gsEntType_t ClassType( const char *classname ) {
	for ( size_t i = 0; i < ARRAY_LEN( classTypes ); i++ ) {
		if ( !Q_stricmp( classTypes[i].classname, classname ) ) {
			return classTypes[i].type;
		}
	}
	// weapons and ammo only matter if the player fires
	if ( !Q_stricmpn( classname, "weapon_", 7 ) || !Q_stricmpn( classname, "ammo_", 5 ) ) {
		return GE_IGNORED;
	}
	return GE_UNSUPPORTED;
}

// Look the powerup up in bg_itemlist. It is linked later, once it has dropped to
// the floor (FinishSpawningItem in game.c).
static int SetupPowerup( gsEnt_t *ent ) {
	for ( const gitem_t *it = bg_itemlist + 1; it->classname; it++ ) {
		if ( !Q_stricmp( it->classname, ent->classname ) && it->giType == IT_POWERUP ) {
			ent->powerup = it->giTag;
			ent->quantity = ent->count ? ent->count : it->quantity;	// as in Pickup_Powerup
			ent->slot = gs_world.numItems++;
			return 1;
		}
	}
	return 0;
}

static void ParseVector( const char *s, vec3_t v ) {
	VectorClear( v );
	sscanf( s, "%f %f %f", &v[0], &v[1], &v[2] );
}

// Read one "{ key value ... }" block. Returns 1 on success, 0 at the end of the
// string, -1 on a syntax error. `random` gets the item respawn randomness key.
static int ParseEntity( const char **p, gsEnt_t *ent, float *random ) {
	const char *tok = COM_Parse( p );
	if ( !tok[0] ) {
		return 0;
	}
	if ( tok[0] != '{' ) {
		GS_SetError( "entity string: expected '{', got '%s'", tok );
		return -1;
	}

	memset( ent, 0, sizeof( *ent ) );
	*random = 0;
	while ( 1 ) {
		char key[MAX_TOKEN_CHARS];
		tok = COM_Parse( p );
		if ( tok[0] == '}' ) {
			return 1;
		}
		if ( !tok[0] ) {
			GS_SetError( "entity string: unexpected end" );
			return -1;
		}
		Q_strncpyz( key, tok, sizeof( key ) );
		tok = COM_Parse( p );

		if ( !Q_stricmp( key, "classname" ) ) {
			Q_strncpyz( ent->classname, tok, sizeof( ent->classname ) );
		} else if ( !Q_stricmp( key, "target" ) ) {
			Q_strncpyz( ent->target, tok, sizeof( ent->target ) );
		} else if ( !Q_stricmp( key, "targetname" ) ) {
			Q_strncpyz( ent->targetname, tok, sizeof( ent->targetname ) );
		} else if ( !Q_stricmp( key, "origin" ) ) {
			ParseVector( tok, ent->origin );
		} else if ( !Q_stricmp( key, "angles" ) ) {
			ParseVector( tok, ent->angles );
		} else if ( !Q_stricmp( key, "angle" ) ) {
			// as in G_SpawnGEntityFromSpawnVars: "angle" sets the yaw only
			VectorSet( ent->angles, 0, atof( tok ), 0 );
		} else if ( !Q_stricmp( key, "model" ) && tok[0] == '*' ) {
			ent->model = CM_InlineModel( atoi( tok + 1 ) );
		} else if ( !Q_stricmp( key, "spawnflags" ) ) {
			ent->spawnflags = atoi( tok );
		} else if ( !Q_stricmp( key, "wait" ) ) {
			ent->wait = atof( tok );
		} else if ( !Q_stricmp( key, "speed" ) ) {
			ent->speed = atof( tok );
		} else if ( !Q_stricmp( key, "count" ) ) {
			ent->count = atoi( tok );
		} else if ( !Q_stricmp( key, "random" ) ) {
			*random = atof( tok );
		}
	}
}

// Classify an entity, give items and checkpoints their per-player slot, and link
// brush models (SV_SetBrushModel + SV_LinkEntity).
static void SetupEntity( gsEnt_t *ent, float random ) {
	ent->type = ClassType( ent->classname );

	if ( ent->type == GE_ITEM_POWERUP ) {
		// a random respawn time can't be reproduced, so such maps aren't trusted
		if ( random || gs_world.numItems == GS_MAX_ITEMS || !SetupPowerup( ent ) ) {
			ent->type = GE_UNSUPPORTED;
		}
	}
	if ( ent->type == GE_TARGET_CHECKPOINT ) {
		if ( gs_world.numCheckpoints == GS_MAX_CHECKPOINTS ) {
			ent->type = GE_UNSUPPORTED;
		} else {
			ent->slot = gs_world.numCheckpoints++;
		}
	}

	if ( ent->model ) {
		CM_ModelBounds( ent->model, ent->mins, ent->maxs );
		VectorAdd( ent->origin, ent->mins, ent->absmin );
		VectorAdd( ent->origin, ent->maxs, ent->absmax );
		for ( int i = 0; i < 3; i++ ) {
			ent->absmin[i] -= 1;
			ent->absmax[i] += 1;
		}
		switch ( ent->type ) {
		case GE_TRIGGER_MULTIPLE:
		case GE_TRIGGER_TELEPORT:
		case GE_TRIGGER_PUSH:
		case GE_TRIGGER_HURT:
			ent->contents = CONTENTS_TRIGGER;	// InitTrigger
			break;
		case GE_SOLID:
			ent->contents = -1;					// SV_SetBrushModel: "we don't know exactly"
			break;
		default:
			break;
		}
	}
}

static int ParseEntities( void ) {
	const char *p = CM_EntityString();
	int hasStart = 0, hasStop = 0;
	float random;

	gs_world.numEnts = 0;
	gs_world.spawnEnt = -1;
	gs_world.numUnsupported = 0;
	gs_world.numItems = 0;
	gs_world.numCheckpoints = 0;

	while ( 1 ) {
		gsEnt_t ent;
		int r = ParseEntity( &p, &ent, &random );
		if ( r < 0 ) {
			return -1;
		}
		if ( r == 0 ) {
			break;
		}
		if ( gs_world.numEnts == GS_MAX_ENTITIES ) {
			GS_SetError( "too many entities" );
			return -1;
		}
		SetupEntity( &ent, random );
		gs_world.ents[ gs_world.numEnts++ ] = ent;

		gs_world.numUnsupported += ent.type == GE_UNSUPPORTED;
		hasStart |= ent.type == GE_TARGET_START_TIMER;
		hasStop |= ent.type == GE_TARGET_STOP_TIMER;
		if ( ent.type == GE_SPAWN && gs_world.spawnEnt < 0 ) {
			gs_world.spawnEnt = gs_world.numEnts - 1;	// DeFRaG maps have one
		}
	}
	gs_world.hasTimer = hasStart && hasStop;

	// resolve target -> targetname links
	for ( int i = 0; i < gs_world.numEnts; i++ ) {
		gsEnt_t *ent = &gs_world.ents[i];
		for ( int j = 0; ent->target[0] && j < gs_world.numEnts && ent->numTargets < GS_MAX_TARGETS; j++ ) {
			if ( !Q_stricmp( gs_world.ents[j].targetname, ent->target ) ) {
				ent->targets[ ent->numTargets++ ] = j;
			}
		}
	}
	return 0;
}

int gs_num_entities( void ) {
	return gs_world.numEnts;
}

int gs_entity( int index, gs_entity_t *out ) {
	if ( index < 0 || index >= gs_world.numEnts ) {
		return -1;
	}
	const gsEnt_t *ent = &gs_world.ents[index];
	memset( out, 0, sizeof( *out ) );
	Q_strncpyz( out->classname, ent->classname, sizeof( out->classname ) );
	Q_strncpyz( out->target, ent->target, sizeof( out->target ) );
	Q_strncpyz( out->targetname, ent->targetname, sizeof( out->targetname ) );
	VectorCopy( ent->origin, out->origin );
	VectorCopy( ent->contents ? ent->absmin : ent->origin, out->absmin );
	VectorCopy( ent->contents ? ent->absmax : ent->origin, out->absmax );
	out->supported = ent->type != GE_UNSUPPORTED;
	return 0;
}

// ---------------------------------------------------------------------------
// loading

int gs_load_map( const char *basepath, const char *game, const char *map ) {
	char name[MAX_QPATH];
	void *data;
	int len, checksum;
	jmp_buf jb;

	gs_error[0] = 0;
	gs_world.loaded = 0;
	snprintf( name, sizeof( name ), "maps/%s.bsp", map );
	if ( !GS_ReadPk3File( basepath, game, name, &data, &len, gs_world.source, sizeof( gs_world.source ) ) ) {
		GS_SetError( "%s not found in %s/%s or %s/baseq3", name, basepath, game, basepath );
		return -1;
	}

	// a broken BSP makes CM_LoadMap call Com_Error, which jumps back here
	if ( setjmp( jb ) ) {
		GS_SetErrorJump( NULL );
		return -1;
	}
	GS_SetErrorJump( &jb );
	GS_HunkClear();
	GS_SetPendingFile( data, len );	// CM_LoadMap reads it through FS_ReadFile
	CM_LoadMap( name, qfalse, &checksum );
	GS_BumpMapGeneration();
	GS_SetErrorJump( NULL );

	Q_strncpyz( gs_world.mapname, map, sizeof( gs_world.mapname ) );
	if ( ParseEntities() < 0 ) {
		return -1;
	}
	G_InitEntities();
	gs_world.loaded = 1;
	return 0;
}

void gs_map_info( gs_map_info_t *info ) {
	memset( info, 0, sizeof( *info ) );
	if ( !gs_world.loaded ) {
		return;
	}
	Q_strncpyz( info->mapname, gs_world.mapname, sizeof( info->mapname ) );
	Q_strncpyz( info->source, gs_world.source, sizeof( info->source ) );
	info->num_entities = gs_world.numEnts;
	info->num_unsupported = gs_world.numUnsupported;
	info->has_timer = gs_world.hasTimer;
	if ( gs_world.spawnEnt >= 0 ) {
		VectorCopy( gs_world.ents[ gs_world.spawnEnt ].origin, info->spawn_origin );
		VectorCopy( gs_world.ents[ gs_world.spawnEnt ].angles, info->spawn_angles );
	}
	CM_ModelBounds( 0, info->world_mins, info->world_maxs );
}

// ---------------------------------------------------------------------------
// world queries (sv_world.c). The real server keeps entities in an area tree; with
// a few hundred entities a plain loop is just as good.

// SV_AreaEntities: linked entities whose bounds overlap the box
int GS_EntitiesInBox( const vec3_t mins, const vec3_t maxs, int *list, int maxcount ) {
	int count = 0;
	for ( int i = 0; i < gs_world.numEnts && count < maxcount; i++ ) {
		const gsEnt_t *e = &gs_world.ents[i];
		if ( !e->contents ) {
			continue;
		}
		if ( e->absmin[0] > maxs[0] || e->absmin[1] > maxs[1] || e->absmin[2] > maxs[2]
			|| e->absmax[0] < mins[0] || e->absmax[1] < mins[1] || e->absmax[2] < mins[2] ) {
			continue;
		}
		list[count++] = i;
	}
	return count;
}

// SV_EntityContact: does the box overlap the entity's brush model?
int GS_EntityContact( const vec3_t mins, const vec3_t maxs, const gsEnt_t *ent ) {
	trace_t trace;
	CM_TransformedBoxTrace( &trace, vec3_origin, vec3_origin, mins, maxs, ent->model, -1,
							ent->origin, ent->angles, qfalse );
	return trace.startsolid;
}

// SV_Trace: the world, then the solid brush entities (SV_ClipMoveToEntities).
// passEntityNum is part of pmove's trace signature; only the player moves, so it's unused.
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

	// the bounding box of the whole move
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
			// make sure we keep a startsolid from a previous trace
			qboolean oldStart = results->startsolid;

			trace.entityNum = GS_FIRST_ENTITY + touch[i];
			*results = trace;
			results->startsolid |= oldStart;
		}
	}
}

// SV_PointContents: the world's contents at a point, plus any brush entity's
int GS_PointContents( const vec3_t p, int passEntityNum ) {
	int touch[GS_MAX_ENTITIES];
	int contents = CM_PointContents( p, 0 );
	int num = GS_EntitiesInBox( p, p, touch, GS_MAX_ENTITIES );
	for ( int i = 0; i < num; i++ ) {
		const gsEnt_t *e = &gs_world.ents[ touch[i] ];
		if ( e->model ) {
			contents |= CM_TransformedPointContents( p, e->model, e->origin, e->angles );
		}
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
