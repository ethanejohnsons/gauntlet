// test_items -- haste pickup and expiry (df_vq3x-1), and jump pads that aim at
// target_push entities (df_vq3x-2)

#include "common.h"

static void test_haste( void ) {
	gs_state_t s;
	float maxSpeed = 0;

	load_or_die( "df_vq3x-1" );
	gs_env_t *env = gs_env_new();
	gs_env_spawn( env );
	gs_env_get_state( env, &s );
	float yaw = s.viewangles[1];

	// two haste items lie 160 units ahead of the spawn; walk through them and keep going
	for ( int f = 0; f < 125; f++ ) {
		gs_cmd_t cmd = { 0 };
		cmd.angles[1] = cmd_angle( yaw, s.delta_angles[1] );
		cmd.forward = 127;
		gs_env_step( env, &cmd );
		gs_env_get_state( env, &s );
		if ( s.ground_entity >= 0 && hspeed( &s ) > maxSpeed ) {
			maxSpeed = hspeed( &s );
		}
	}
	printf( "haste: %d ms left, max ground speed %.1f ups\n", s.haste_ms, maxSpeed );
	// both items are within reach at once, 30 s each
	CHECK( s.haste_ms > 50000 && s.haste_ms <= 60000, "should have picked up both hastes, %d ms left", s.haste_ms );
	CHECK( maxSpeed > 400 && maxSpeed <= 416.5f, "haste ground speed should be 320 * 1.3 = 416, got %.1f", maxSpeed );

	// these items respawn every second, so step away from them and wait for it to run out
	gs_map_info_t info;
	gs_map_info( &info );
	float back[3] = { info.spawn_origin[0], info.spawn_origin[1], info.spawn_origin[2] + 9 };
	float zero[3] = { 0, 0, 0 };
	gs_env_place( env, back, zero, yaw );
	for ( int f = 0; f < 125 * 61; f++ ) {
		gs_cmd_t cmd = { 0 };
		cmd.angles[1] = cmd_angle( yaw, s.delta_angles[1] );
		gs_env_step( env, &cmd );
	}
	gs_env_get_state( env, &s );
	CHECK( s.haste_ms == 0, "haste should have expired, %d ms left", s.haste_ms );
	gs_env_free( env );
}

static void test_push_to_target_push( void ) {
	gs_entity_t pad, aim;
	gs_state_t s;
	int found = 0;

	load_or_die( "df_vq3x-2" );
	// the trigger_push whose target is target_push "t12"
	for ( int i = 0; i < gs_num_entities() && !found; i++ ) {
		gs_entity( i, &pad );
		if ( !strcmp( pad.classname, "trigger_push" ) && !strcmp( pad.target, "t12" ) ) {
			found = 1;
		}
	}
	CHECK( found, "no trigger_push targeting t12" );
	found = 0;
	for ( int i = 0; i < gs_num_entities() && !found; i++ ) {
		gs_entity( i, &aim );
		found = !strcmp( aim.targetname, "t12" );
	}
	CHECK( found && !strcmp( aim.classname, "target_push" ), "t12 should be a target_push" );

	// expected launch velocity: AimAtTarget from the pad's center to the target
	float center[3], dir[3];
	for ( int i = 0; i < 3; i++ ) {
		center[i] = ( pad.absmin[i] + pad.absmax[i] ) * 0.5f;
	}
	float height = aim.origin[2] - center[2];
	float time = sqrtf( height / ( 0.5f * 800 ) );
	dir[0] = aim.origin[0] - center[0];
	dir[1] = aim.origin[1] - center[1];
	float dist = sqrtf( dir[0] * dir[0] + dir[1] * dir[1] );
	float expect[3] = { dir[0] / dist * dist / time, dir[1] / dist * dist / time, time * 800 };

	// stand in the pad (its brush starts one unit above absmin)
	gs_env_t *env = gs_env_new();
	gs_env_spawn( env );
	float origin[3] = { center[0], center[1], pad.absmin[2] + 1 + 24.125f };
	float zero[3] = { 0, 0, 0 };
	gs_env_place( env, origin, zero, 0 );
	gs_cmd_t cmd = { 0 };
	gs_env_step( env, &cmd );
	gs_env_get_state( env, &s );

	printf( "jump pad -> target_push: velocity (%.2f %.2f %.2f), expected (%.2f %.2f %.2f)\n",
			s.velocity[0], s.velocity[1], s.velocity[2], expect[0], expect[1], expect[2] );
	for ( int i = 0; i < 3; i++ ) {
		CHECK( fabsf( s.velocity[i] - expect[i] ) < 0.5f, "launch velocity[%d] off", i );
	}
	gs_env_free( env );
}

int main( void ) {
	test_haste();
	test_push_to_target_push();
	printf( "ok\n" );
	return 0;
}
