"""Scripted bots, for testing the sim and the replay pipeline before there's any AI."""

import math

from .sim import BUTTON_ATTACK, WP_ROCKET_LAUNCHER, Cmd, cmd_angle


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


class RocketJumpBot:
    """Walks to a spot (where a rocket launcher is), then rocket jumps forward again
    and again: jump, look straight down and fire on the first frame in the air.
    For checking rockets, splash damage and knockback against the real engine."""

    def __init__(self, goal=None, jumps=4):
        if goal is None:    # the map's first rocket launcher
            from .sim import entities
            goal = next(e["origin"] for e in entities() if e["classname"] == "weapon_rocketlauncher")
        self.goal = goal
        self.jumps = jumps
        self.phase = "walk"
        self.wait = 0
        self.yaw = None

    def __call__(self, s):
        cmd = Cmd()
        have_rl = bool(s.weapons & (1 << WP_ROCKET_LAUNCHER))
        if self.phase == "walk":
            dx, dy = self.goal[0] - s.origin[0], self.goal[1] - s.origin[1]
            self.yaw = math.degrees(math.atan2(dy, dx))
            cmd.angles[1] = cmd_angle(self.yaw, s.delta_angles[1])
            cmd.forward = 127
            if have_rl:
                self.phase, self.wait = "raise", 0
            return cmd
        cmd.weapon = WP_ROCKET_LAUNCHER
        cmd.angles[1] = cmd_angle(self.yaw, s.delta_angles[1])
        if self.phase == "raise":           # stand still while the launcher comes up
            self.wait += 1
            cmd.angles[0] = cmd_angle(0, s.delta_angles[0])
            if self.wait > 60 and s.weapon == WP_ROCKET_LAUNCHER and s.ground_entity >= 0:
                self.phase = "jump"
            return cmd
        cmd.forward = 127
        if self.phase == "jump":            # jump...
            cmd.up = 127
            cmd.angles[0] = cmd_angle(89, s.delta_angles[0])
            if s.ground_entity < 0:
                self.phase = "fire"
            return cmd
        if self.phase == "fire":            # ...and fire at the floor once airborne
            cmd.angles[0] = cmd_angle(89, s.delta_angles[0])
            cmd.buttons = BUTTON_ATTACK
            if s.weapon_time > 0 or s.rockets == 0:
                self.phase, self.jumps = "fly", self.jumps - 1
            return cmd
        # fly: look ahead, and go again after landing while rockets last
        cmd.angles[0] = cmd_angle(0, s.delta_angles[0])
        if s.ground_entity >= 0 and self.jumps > 0 and s.rockets > 0 and s.weapon_time == 0:
            self.phase = "jump"
        return cmd
