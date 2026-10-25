// test_strafe -- a scripted strafe jump must gain speed past the 320 ups ground cap,
// which only works if air acceleration, jumping and friction behave like VQ3

#include "common.h"

#define DEG( r )	( (r) * 180.0f / (float)M_PI )
#define RAD( d )	( (d) * (float)M_PI / 180.0f )

// One frame of VQ3 strafe jumping: wishdir held at the optimal air angle to the velocity.
static gs_cmd_t strafe_cmd( const gs_state_t *s, int side, int *jumpHeld ) {
	gs_cmd_t cmd = { 0 };
	float speed = hspeed( s );
	float velYaw = DEG( atan2f( s->velocity[1], s->velocity[0] ) );
	// optimal: v . wishdir = wishspeed - accel * wishspeed * frametime (air accel 1, 320 ups)
	float best = 320.0f - 320.0f * 0.008f;
	float theta = speed > best ? DEG( acosf( best / speed ) ) : 0.0f;
	// forward + right puts wishdir 45 degrees right of the view, forward + left 45 left
	float view = side > 0 ? velYaw - theta + 45.0f : velYaw + theta - 45.0f;

	cmd.angles[1] = cmd_angle( view, s->delta_angles[1] );
	cmd.forward = 127;
	cmd.right = side > 0 ? 127 : -127;
	// tap jump on landing; it has to be released in the air to jump again
	if ( s->ground_entity >= 0 && !*jumpHeld ) {
		cmd.up = 127;
		*jumpHeld = 1;
	} else if ( s->ground_entity < 0 ) {
		*jumpHeld = 0;
	}
	return cmd;
}

int main( int argc, char **argv ) {
	const char *map = argc > 1 ? argv[1] : "vanilla_01";
	gs_state_t s;
	float maxSpeed = 0;
	int jumpHeld = 0, jumps = 0, wasGround = 1;

	load_or_die( map );
	gs_env_t *env = gs_env_new();
	gs_env_spawn( env );
	gs_env_get_state( env, &s );
	float startYaw = s.viewangles[1];
	float start[3] = { s.origin[0], s.origin[1], s.origin[2] };

	// run straight ahead for 0.3 s, then strafe jump, switching sides every 0.4 s
	for ( int f = 0; f < 38; f++ ) {
		gs_cmd_t cmd = { 0 };
		cmd.angles[1] = cmd_angle( startYaw, s.delta_angles[1] );
		cmd.forward = 127;
		gs_env_step( env, &cmd );
		gs_env_get_state( env, &s );
	}
	printf( "after run-up: %.1f ups\n", hspeed( &s ) );
	CHECK( hspeed( &s ) > 300 && hspeed( &s ) <= 320.5f, "ground run should reach ~320 ups, got %.1f", hspeed( &s ) );

	for ( int f = 0; f < 250 && !s.dead; f++ ) {
		int side = ( f / 50 ) % 2 ? -1 : 1;
		gs_cmd_t cmd = strafe_cmd( &s, side, &jumpHeld );
		gs_env_step( env, &cmd );
		gs_env_get_state( env, &s );
		if ( wasGround && s.ground_entity < 0 ) {
			jumps++;
		}
		wasGround = s.ground_entity >= 0;
		if ( hspeed( &s ) > maxSpeed ) {
			maxSpeed = hspeed( &s );
		}
		if ( f % 25 == 0 ) {
			printf( "  f %3d  speed %6.1f  pos (%7.1f %7.1f %6.1f)  %s\n", f, hspeed( &s ),
					s.origin[0], s.origin[1], s.origin[2], s.ground_entity >= 0 ? "ground" : "air" );
		}
	}
	float dx = s.origin[0] - start[0], dy = s.origin[1] - start[1];
	printf( "max speed %.1f ups, %d jumps, moved %.0f units\n", maxSpeed, jumps, sqrtf( dx * dx + dy * dy ) );
	CHECK( jumps >= 3, "should have jumped repeatedly, got %d", jumps );
	CHECK( maxSpeed > 380, "strafe jumping should exceed 380 ups, got %.1f", maxSpeed );
	gs_env_free( env );
	printf( "ok\n" );
	return 0;
}
