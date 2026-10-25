// test_rocket -- pick up the rocket launcher on vanilla_01 and rocket jump: the
// explosion must launch the player upwards and cost about half its splash damage

#include "common.h"

int main( int argc, char **argv ) {
	gs_entity_t ent;
	gs_state_t s;
	float goal[3];
	int found = 0;

	load_or_die( "vanilla_01" );
	for ( int i = 0; gs_entity( i, &ent ) == 0 && !found; i++ ) {
		if ( !strcmp( ent.classname, "weapon_rocketlauncher" ) ) {
			memcpy( goal, ent.origin, sizeof( goal ) );
			found = 1;
		}
	}
	CHECK( found, "vanilla_01 should have a rocket launcher" );

	gs_env_t *env = gs_env_new();
	gs_env_spawn( env );
	gs_env_get_state( env, &s );

	// walk to the launcher (the spawn point makes it appear)
	for ( int f = 0; f < 300 && !( s.weapons & ( 1 << GS_WP_ROCKET_LAUNCHER ) ); f++ ) {
		gs_cmd_t cmd = { 0 };
		float yaw = atan2f( goal[1] - s.origin[1], goal[0] - s.origin[0] ) * 180 / (float)M_PI;
		cmd.angles[1] = cmd_angle( yaw, s.delta_angles[1] );
		cmd.forward = 127;
		gs_env_step( env, &cmd );
		gs_env_get_state( env, &s );
	}
	CHECK( s.weapons & ( 1 << GS_WP_ROCKET_LAUNCHER ), "should have picked up the rocket launcher" );
	CHECK( s.rockets > 0, "should have rockets" );

	// switch to it and stand still until it's ready
	for ( int f = 0; f < 80; f++ ) {
		gs_cmd_t cmd = { 0 };
		cmd.weapon = GS_WP_ROCKET_LAUNCHER;
		gs_env_step( env, &cmd );
		gs_env_get_state( env, &s );
	}
	CHECK( s.weapon == GS_WP_ROCKET_LAUNCHER && s.weapon_time == 0, "rocket launcher should be up" );

	// jump, then fire straight down on the first frame in the air
	int health = s.health, rockets = s.rockets;
	float maxUp = 0;
	for ( int f = 0; f < 60; f++ ) {
		gs_cmd_t cmd = { 0 };
		cmd.weapon = GS_WP_ROCKET_LAUNCHER;
		cmd.angles[0] = cmd_angle( 89, s.delta_angles[0] );
		cmd.up = f == 0 ? 127 : 0;
		cmd.buttons = f > 0 && s.rockets == rockets ? GS_BUTTON_ATTACK : 0;
		gs_env_step( env, &cmd );
		gs_env_get_state( env, &s );
		maxUp = s.velocity[2] > maxUp ? s.velocity[2] : maxUp;
	}
	printf( "rocket jump: up to %.0f ups upwards, health %d -> %d, rockets %d -> %d\n",
			maxUp, health, s.health, rockets, s.rockets );
	CHECK( s.rockets == rockets - 1, "one rocket should have been fired" );
	CHECK( maxUp > 600, "the explosion should launch the player (a jump alone is 270 ups)" );
	CHECK( health - s.health >= 30 && health - s.health <= 50, "self damage should be halved, about 30-50" );
	gs_env_free( env );
	printf( "ok\n" );
	return 0;
}
