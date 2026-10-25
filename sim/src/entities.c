// entities.c -- parse the map's entity string into gs_world.ents: classify each entity,
// link brush models like SV_LinkEntity, and resolve target -> targetname links.

#include "sim_local.h"

typedef struct {
	const char	*classname;
	gsEntType_t	type;
} classMap_t;

// Entities that matter for movement, and the harmless ones. Anything else counts as
// unsupported, so a map is only trusted when everything in it is understood.
static const classMap_t classMap[] = {
	{ "worldspawn",				GE_WORLDSPAWN },
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

	// no effect on movement while the player never fires
	{ "light",					GE_IGNORED },
	{ "misc_model",				GE_IGNORED },
	{ "info_null",				GE_IGNORED },
	{ "info_player_intermission", GE_IGNORED },
	{ "target_speaker",			GE_IGNORED },
	{ "target_print",			GE_IGNORED },
	{ "target_smallprint",		GE_IGNORED },
	{ "target_location",		GE_IGNORED },
	{ "target_init",			GE_IGNORED },
	{ "target_give",			GE_IGNORED },
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
	{ NULL,						GE_UNSUPPORTED },
};

static gsEntType_t ClassType( const char *classname ) {
	for ( const classMap_t *m = classMap; m->classname; m++ ) {
		if ( !Q_stricmp( m->classname, classname ) ) {
			return m->type;
		}
	}
	// weapons and ammo only matter if the player fires
	if ( !Q_stricmpn( classname, "weapon_", 7 ) || !Q_stricmpn( classname, "ammo_", 5 ) ) {
		return GE_IGNORED;
	}
	return GE_UNSUPPORTED;
}

// Fill in a powerup item from bg_itemlist. The item is linked later, once it has
// dropped to the floor (G_InitEntities).
static int SetupPowerup( gsEnt_t *ent ) {
	for ( const gitem_t *it = bg_itemlist + 1; it->classname; it++ ) {
		if ( !Q_stricmp( it->classname, ent->classname ) && it->giType == IT_POWERUP ) {
			ent->powerup = it->giTag;
			ent->quantity = ent->count ? ent->count : it->quantity;	// Pickup_Powerup
			ent->itemSlot = gs_world.numItems++;
			return 1;
		}
	}
	return 0;
}

static void ParseVector( const char *s, vec3_t v ) {
	VectorClear( v );
	sscanf( s, "%f %f %f", &v[0], &v[1], &v[2] );
}

int GS_ParseEntities( void ) {
	const char *p = CM_EntityString();
	gsEnt_t *ent;

	gs_world.numEnts = 0;
	gs_world.spawnEnt = -1;
	gs_world.numUnsupported = 0;
	gs_world.numItems = 0;

	while ( 1 ) {
		const char *tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		if ( tok[0] != '{' ) {
			GS_SetError( "entity string: expected '{', got '%s'", tok );
			return -1;
		}
		if ( gs_world.numEnts == GS_MAX_ENTITIES ) {
			GS_SetError( "too many entities" );
			return -1;
		}
		ent = &gs_world.ents[ gs_world.numEnts++ ];
		memset( ent, 0, sizeof( *ent ) );
		float random = 0;

		while ( 1 ) {
			char key[MAX_TOKEN_CHARS];
			tok = COM_Parse( &p );
			if ( tok[0] == '}' ) {
				break;
			}
			if ( !tok[0] ) {
				GS_SetError( "entity string: unexpected end" );
				return -1;
			}
			Q_strncpyz( key, tok, sizeof( key ) );
			tok = COM_Parse( &p );

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
				// G_SpawnGEntityFromSpawnVars: "angle" sets the yaw only
				VectorClear( ent->angles );
				ent->angles[YAW] = atof( tok );
			} else if ( !Q_stricmp( key, "model" ) && tok[0] == '*' ) {
				ent->model = CM_InlineModel( atoi( tok + 1 ) );
			} else if ( !Q_stricmp( key, "spawnflags" ) ) {
				ent->spawnflags = atoi( tok );
			} else if ( !Q_stricmp( key, "dmg" ) ) {
				ent->dmg = atoi( tok );
			} else if ( !Q_stricmp( key, "wait" ) ) {
				ent->wait = atof( tok );
			} else if ( !Q_stricmp( key, "speed" ) ) {
				ent->speed = atof( tok );
			} else if ( !Q_stricmp( key, "count" ) ) {
				ent->count = atoi( tok );
			} else if ( !Q_stricmp( key, "random" ) ) {
				random = atof( tok );
			}
		}

		ent->type = ClassType( ent->classname );
		if ( ent->type == GE_ITEM_POWERUP ) {
			// a random respawn time can't be reproduced, so such maps aren't trusted
			if ( random || gs_world.numItems == GS_MAX_ITEMS || !SetupPowerup( ent ) ) {
				ent->type = GE_UNSUPPORTED;
			}
		}
		if ( ent->type == GE_UNSUPPORTED ) {
			gs_world.numUnsupported++;
		}

		// brush models: link like SV_LinkEntity (bounds padded by one unit)
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
				ent->contents = -1;					// SV_SetBrushModel
				break;
			default:
				ent->contents = 0;
				break;
			}
		}

		if ( ent->type == GE_SPAWN && gs_world.spawnEnt < 0 ) {
			gs_world.spawnEnt = gs_world.numEnts - 1;
		}
	}

	// resolve targets and check that a timer exists
	int hasStart = 0, hasStop = 0;
	for ( int i = 0; i < gs_world.numEnts; i++ ) {
		ent = &gs_world.ents[i];
		if ( ent->type == GE_TARGET_START_TIMER ) {
			hasStart = 1;
		}
		if ( ent->type == GE_TARGET_STOP_TIMER ) {
			hasStop = 1;
		}
		if ( !ent->target[0] ) {
			continue;
		}
		for ( int j = 0; j < gs_world.numEnts; j++ ) {
			if ( !Q_stricmp( gs_world.ents[j].targetname, ent->target ) && ent->numTargets < GS_MAX_TARGETS ) {
				ent->targets[ ent->numTargets++ ] = j;
			}
		}
	}
	gs_world.hasTimer = hasStart && hasStop;
	return 0;
}

int gs_unsupported_entities( char *buf, int buflen ) {
	int len = 0;
	if ( buflen > 0 ) {
		buf[0] = 0;
	}
	for ( int i = 0; i < gs_world.numEnts && len < buflen; i++ ) {
		const gsEnt_t *ent = &gs_world.ents[i];
		if ( ent->type == GE_UNSUPPORTED ) {
			len += snprintf( buf + len, buflen - len, "%s (%.0f %.0f %.0f)\n", ent->classname,
							 ent->origin[0], ent->origin[1], ent->origin[2] );
		}
	}
	return len < buflen ? len : buflen - 1;
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
	if ( ent->contents ) {
		VectorCopy( ent->absmin, out->absmin );
		VectorCopy( ent->absmax, out->absmax );
	} else {
		VectorCopy( ent->origin, out->absmin );
		VectorCopy( ent->origin, out->absmax );
	}
	out->supported = ent->type != GE_UNSUPPORTED;
	return 0;
}
