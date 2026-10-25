// combat.c -- the rocket launcher: firing, rockets in flight, splash damage and the
// knockback that makes rocket jumping work. Rewritten from id's 1.32 g_weapon.c
// (CalcMuzzlePoint, fire_rocket), g_missile.c (G_RunMissile, G_MissileImpact,
// G_ExplodeMissile) and g_combat.c (G_Damage, G_RadiusDamage, CanDamage, CheckArmor).
//
// Only the player can be damaged (each env races alone), and only the rocket
// launcher is simulated; other weapons fire nothing here, which is right for movement
// as long as their shots can't push the player.
//
// Compiled like the QVM (see q3/game/bg_vmcompat.h), like game.c.

#include "sim_local.h"

#define MISSILE_PRESTEP_TIME	50		// g_local.h: rockets start this far along their path
#define ROCKET_SPEED			900
#define ROCKET_SPLASH_DAMAGE	100
#define ROCKET_SPLASH_RADIUS	120
#define ROCKET_LIFETIME			15000
#define KNOCKBACK				1000	// g_knockback
#define PLAYER_MASS				200

/*
=================
G_Damage (g_combat.c), for the player

dir (may be NULL) is where the push comes from; it is normalized here. Knockback is
based on the full damage; then self-inflicted damage is halved, armor takes its share
and the rest comes off health.
=================
*/
void G_Damage( gs_env_t *env, vec3_t dir, int damage, int dflags, int selfInflicted ) {
	playerState_t *ps = &env->ps;
	int			take, asave, knockback;

	if ( env->dead ) {
		return;
	}
	if ( !dir ) {
		dflags |= DAMAGE_NO_KNOCKBACK;
	} else {
		VectorNormalize( dir );
	}

	knockback = damage;
	if ( knockback > 200 ) {
		knockback = 200;
	}
	if ( dflags & DAMAGE_NO_KNOCKBACK ) {
		knockback = 0;
	}

	// figure momentum add, even if the damage won't be taken
	if ( knockback ) {
		vec3_t	kvel;
		float	mass = PLAYER_MASS;

		VectorScale( dir, KNOCKBACK * (float)knockback / mass, kvel );
		VectorAdd( ps->velocity, kvel, ps->velocity );

		// set the timer so that the other client can't cancel
		// out the movement immediately
		if ( !ps->pm_time ) {
			int		t;

			t = knockback * 2;
			if ( t < 50 ) {
				t = 50;
			}
			if ( t > 200 ) {
				t = 200;
			}
			ps->pm_time = t;
			ps->pm_flags |= PMF_TIME_KNOCKBACK;
		}
	}

	// battlesuit protects from all radius damage (but takes knockback)
	if ( ps->powerups[PW_BATTLESUIT] ) {
		if ( dflags & DAMAGE_RADIUS ) {
			return;
		}
		damage *= 0.5;
	}

	// rocket jumping: you take half of your own damage
	if ( selfInflicted ) {
		damage *= 0.5;
	}
	if ( damage < 1 ) {
		damage = 1;
	}
	take = damage;

	// CheckArmor
	asave = 0;
	if ( !( dflags & DAMAGE_NO_ARMOR ) && ps->stats[STAT_ARMOR] ) {
		asave = ceil( take * ARMOR_PROTECTION );
		if ( asave >= ps->stats[STAT_ARMOR] ) {
			asave = ps->stats[STAT_ARMOR];
		}
		ps->stats[STAT_ARMOR] -= asave;
	}
	take -= asave;

	if ( take ) {
		ps->stats[STAT_HEALTH] -= take;
		if ( ps->stats[STAT_HEALTH] <= 0 ) {
			env->dead = 1;
		}
	}
}

/*
=================
CanDamage (g_combat.c)

Can the explosion at origin reach the player? Traces to the middle of its bounds and
to four points around it.
=================
*/
static int CanDamage( const gs_env_t *env, const vec3_t origin ) {
	static const float offsets[5][2] = { { 0, 0 }, { 15, 15 }, { 15, -15 }, { -15, 15 }, { -15, -15 } };
	vec3_t	midpoint, dest;
	trace_t	tr;

	// use the midpoint of the bounds instead of the origin, because
	// bmodels may have their origin is 0,0,0
	VectorAdd( env->linkMins, env->linkMaxs, midpoint );
	VectorScale( midpoint, 0.5, midpoint );

	for ( int i = 0; i < 5; i++ ) {
		VectorCopy( midpoint, dest );
		dest[0] += offsets[i][0];
		dest[1] += offsets[i][1];
		GS_Trace( &tr, origin, vec3_origin, vec3_origin, dest, ENTITYNUM_NONE, MASK_SOLID );
		if ( tr.fraction == 1.0 ) {
			return 1;
		}
	}
	return 0;
}

/*
=================
G_RadiusDamage (g_combat.c), for the player
=================
*/
static void G_RadiusDamage( gs_env_t *env, const vec3_t origin, float damage, float radius ) {
	float		points, dist;
	vec3_t		v, dir;
	int			i;

	if ( env->dead ) {
		return;
	}
	// trap_EntitiesInBox: is the player's linked box within reach at all?
	for ( i = 0 ; i < 3 ; i++ ) {
		if ( env->linkMins[i] > origin[i] + radius || env->linkMaxs[i] < origin[i] - radius ) {
			return;
		}
	}

	// find the distance from the edge of the bounding box
	for ( i = 0 ; i < 3 ; i++ ) {
		if ( origin[i] < env->linkMins[i] ) {
			v[i] = env->linkMins[i] - origin[i];
		} else if ( origin[i] > env->linkMaxs[i] ) {
			v[i] = origin[i] - env->linkMaxs[i];
		} else {
			v[i] = 0;
		}
	}

	dist = VectorLength( v );
	if ( dist >= radius ) {
		return;
	}

	points = damage * ( 1.0 - dist / radius );

	if ( CanDamage( env, origin ) ) {
		// r.currentOrigin: the exact origin, copied back after the triggers
		VectorSubtract( env->ps.origin, origin, dir );
		// push the center of mass higher than the origin so players
		// get knocked into the air more
		dir[2] += 24;
		G_Damage( env, dir, (int)points, DAMAGE_RADIUS, 1 );
	}
}

/*
=================
G_FireWeapon (FireWeapon / CalcMuzzlePointOrigin / fire_rocket in g_weapon.c, g_missile.c)

trBase is the player's snapped origin (ent->s.pos.trBase), which the muzzle starts from.
=================
*/
void G_FireWeapon( gs_env_t *env, const vec3_t trBase ) {
	playerState_t *ps = &env->ps;
	vec3_t		forward, right, up, muzzle;
	gsMissile_t	*bolt = NULL;

	if ( ps->weapon != WP_ROCKET_LAUNCHER ) {
		return;		// only the rocket launcher is simulated
	}

	// set aiming directions
	AngleVectors( ps->viewangles, forward, right, up );

	// CalcMuzzlePointOrigin
	VectorCopy( trBase, muzzle );
	muzzle[2] += ps->viewheight;
	VectorMA( muzzle, 14, forward, muzzle );
	// snap to integer coordinates for more efficient network bandwidth usage
	SnapVector( muzzle );

	for ( int i = 0; i < GS_MAX_MISSILES; i++ ) {
		if ( !env->missiles[i].active ) {
			bolt = &env->missiles[i];
			break;
		}
	}
	if ( !bolt ) {
		return;		// more rockets in flight than we track; can't happen at 0.8 s per shot
	}

	// fire_rocket
	VectorNormalize( forward );
	memset( bolt, 0, sizeof( *bolt ) );
	bolt->active = 1;
	bolt->explodeTime = env->levelTime + ROCKET_LIFETIME;
	bolt->pos.trType = TR_LINEAR;
	bolt->pos.trTime = env->levelTime - MISSILE_PRESTEP_TIME;		// move a bit on the very first frame
	VectorCopy( muzzle, bolt->pos.trBase );
	VectorScale( forward, ROCKET_SPEED, bolt->pos.trDelta );
	SnapVector( bolt->pos.trDelta );			// save net bandwidth
	VectorCopy( muzzle, bolt->currentOrigin );
}

/*
=================
G_RunMissiles (G_RunMissile / G_MissileImpact / G_ExplodeMissile in g_missile.c)

Move every rocket to where it is at levelTime; explode it on impact, or when its
time is up.
=================
*/
void G_RunMissiles( gs_env_t *env, int levelTime ) {
	for ( int i = 0; i < GS_MAX_MISSILES; i++ ) {
		gsMissile_t	*ent = &env->missiles[i];
		vec3_t		origin;
		trace_t		tr;

		if ( !ent->active ) {
			continue;
		}

		// get current position
		BG_EvaluateTrajectory( &ent->pos, levelTime, origin );

		// trace a line from the previous position to the current position
		// (rockets have no size; the owner is ignored, and is the only player)
		GS_Trace( &tr, ent->currentOrigin, vec3_origin, vec3_origin, origin, GS_PLAYER_NUM, MASK_SHOT );

		if ( tr.startsolid || tr.allsolid ) {
			// make sure the tr.entityNum is set to the entity we're stuck in
			GS_Trace( &tr, ent->currentOrigin, vec3_origin, vec3_origin, ent->currentOrigin, GS_PLAYER_NUM, MASK_SHOT );
			tr.fraction = 0;
		} else {
			VectorCopy( tr.endpos, ent->currentOrigin );
		}

		if ( tr.fraction != 1 ) {
			// never explode or bounce on sky
			if ( tr.surfaceFlags & SURF_NOIMPACT ) {
				ent->active = 0;
				continue;
			}
			// G_MissileImpact: snap towards where it came from, then splash there
			// (SnapVectorTowards in g_weapon.c changes the impact point in place)
			for ( int k = 0 ; k < 3 ; k++ ) {
				if ( ent->pos.trBase[k] <= tr.endpos[k] ) {
					tr.endpos[k] = (int)tr.endpos[k];
				} else {
					tr.endpos[k] = (int)tr.endpos[k] + 1;
				}
			}
			ent->active = 0;
			G_RadiusDamage( env, tr.endpos, ROCKET_SPLASH_DAMAGE, ROCKET_SPLASH_RADIUS );
			continue;
		}

		// G_RunThink: explode in mid air after 15 seconds
		if ( levelTime >= ent->explodeTime ) {
			BG_EvaluateTrajectory( &ent->pos, levelTime, origin );
			SnapVector( origin );
			ent->active = 0;
			G_RadiusDamage( env, origin, ROCKET_SPLASH_DAMAGE, ROCKET_SPLASH_RADIUS );
		}
	}
}
