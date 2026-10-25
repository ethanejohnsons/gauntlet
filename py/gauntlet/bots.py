"""Scripted bots, for testing the sim and the replay pipeline before there's any AI."""

import math

from .sim import Cmd, cmd_angle


def hspeed(s):
    return math.hypot(s.velocity[0], s.velocity[1])


class StrafeBot:
    """VQ3 strafe jumping in a straight-ish line, as in sim/tests/test_strafe.c.

    Runs forward for `runup` frames, then strafe jumps, switching sides every
    `switch` frames. It doesn't steer around anything.
    """

    def __init__(self, runup=38, switch=50):
        self.runup, self.switch = runup, switch
        self.frame = 0
        self.jump_held = False
        self.start_yaw = None

    def __call__(self, s):
        if self.start_yaw is None:
            self.start_yaw = s.viewangles[1]
        cmd = Cmd()
        cmd.forward = 127
        f = self.frame
        self.frame += 1
        if f < self.runup:
            cmd.angles[1] = cmd_angle(self.start_yaw, s.delta_angles[1])
            return cmd

        side = 1 if ((f - self.runup) // self.switch) % 2 == 0 else -1
        speed = hspeed(s)
        vel_yaw = math.degrees(math.atan2(s.velocity[1], s.velocity[0]))
        # optimal: v . wishdir = wishspeed - accel * wishspeed * frametime (air accel 1)
        best = 320 - 320 * 0.008
        theta = math.degrees(math.acos(best / speed)) if speed > best else 0.0
        view = vel_yaw - theta + 45 if side > 0 else vel_yaw + theta - 45
        cmd.angles[1] = cmd_angle(view, s.delta_angles[1])
        cmd.right = 127 if side > 0 else -127
        # tap jump on landing; it has to be released in the air to jump again
        if s.ground_entity >= 0 and not self.jump_held:
            cmd.up, self.jump_held = 127, True
        elif s.ground_entity < 0:
            self.jump_held = False
        return cmd


class Idle:
    """Stand still, looking where the player already looks."""

    def __call__(self, s):
        cmd = Cmd()
        cmd.angles[0] = cmd_angle(s.viewangles[0], s.delta_angles[0])
        cmd.angles[1] = cmd_angle(s.viewangles[1], s.delta_angles[1])
        return cmd
