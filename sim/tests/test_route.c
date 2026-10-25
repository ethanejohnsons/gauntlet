// test_route -- the route must lead from the spawn to the finish: following it
// (gs_route_ahead) keeps getting closer and arrives. The default map has no
// teleporters; walking stops at a teleport trigger, since it can't step through.

#include "common.h"

int main( int argc, char **argv ) {
	const char *map = argc > 1 ? argv[1] : "vanilla_01";
	gs_map_info_t info;

	load_or_die( map );
	gs_map_info( &info );
	gs_set_verbose( argc > 2 );
	CHECK( gs_route_build( 0 ) == 0, "route build failed: %s", gs_last_error() );

	float spawn[3] = { info.spawn_origin[0], info.spawn_origin[1], info.spawn_origin[2] + 9 };
	float d = gs_route_distance( GS_ROUTE_FINISH, spawn );
	printf( "%s: spawn is %.0f units from the start zone and %.0f from the finish\n", map,
			gs_route_distance( GS_ROUTE_START, spawn ), d );
	CHECK( gs_route_distance( GS_ROUTE_START, spawn ) >= 0, "spawn should be connected to the start zone" );
	CHECK( d > 0, "spawn should be connected to the finish" );

	// follow the route from the spawn; every step must get closer, and we must arrive
	float pos[3] = { spawn[0], spawn[1], spawn[2] }, next[3];
	int steps = 0;
	while ( gs_route_distance( GS_ROUTE_FINISH, pos ) > 0 && steps < 10000 ) {
		CHECK( gs_route_ahead( GS_ROUTE_FINISH, pos, 64, next ) == 0, "route_ahead failed at step %d", steps );
		float dn = gs_route_distance( GS_ROUTE_FINISH, next );
		if ( dn >= gs_route_distance( GS_ROUTE_FINISH, pos ) ) {
			break;	// teleport trigger or the end
		}
		pos[0] = next[0]; pos[1] = next[1]; pos[2] = next[2];
		steps++;
	}
	printf( "  walking the route: %d steps of ~64 units, ended %.0f units from the finish at (%.0f %.0f %.0f)\n",
			steps, gs_route_distance( GS_ROUTE_FINISH, pos ), pos[0], pos[1], pos[2] );
	CHECK( gs_route_distance( GS_ROUTE_FINISH, pos ) < 64, "walking the route should reach the finish" );

	printf( "ok\n" );
	return 0;
}
