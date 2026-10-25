// test_map -- load a DeFRaG map, spawn, and check the player settles on the floor

#include "common.h"

int main( int argc, char **argv ) {
	const char *map = argc > 1 ? argv[1] : "vanilla_01";
	gs_map_info_t info;
	gs_entity_t ent;
	gs_state_t s;

	load_or_die( map );
	gs_map_info( &info );
	printf( "%s from %s\n", info.mapname, info.source );
	printf( "  %d entities, timer %s, %d unsupported\n", info.num_entities,
			info.has_timer ? "yes" : "no", info.num_unsupported );
	for ( int i = 0; gs_entity( i, &ent ) == 0; i++ ) {
		if ( !ent.supported ) {
			printf( "    unsupported: %s (%.0f %.0f %.0f)\n", ent.classname,
					ent.origin[0], ent.origin[1], ent.origin[2] );
		}
	}
	printf( "  spawn (%.0f %.0f %.0f) yaw %.0f\n", info.spawn_origin[0], info.spawn_origin[1],
			info.spawn_origin[2], info.spawn_angles[1] );

	gs_env_t *env = gs_env_new();
	gs_env_spawn( env );
	gs_cmd_t cmd = { 0 };
	for ( int i = 0; i < 125; i++ ) {
		gs_env_step( env, &cmd );
	}
	gs_env_get_state( env, &s );
	printf( "  after 1 s idle: origin (%.3f %.3f %.3f) vel (%.3f %.3f %.3f) ground %d\n",
			s.origin[0], s.origin[1], s.origin[2], s.velocity[0], s.velocity[1], s.velocity[2], s.ground_entity );

	CHECK( s.ground_entity >= 0, "player should be on the ground" );
	CHECK( fabsf( s.velocity[2] ) < 1e-3f && hspeed( &s ) < 1e-3f, "player should be at rest" );
	CHECK( fabsf( s.origin[2] - ( info.spawn_origin[2] + 9 ) ) < 40, "player should be near the spawn height" );
	CHECK( s.command_time == 1000 + 125 * GS_FRAME_MSEC, "command time should advance 8 ms per step" );
	gs_env_free( env );
	printf( "ok\n" );
	return 0;
}
