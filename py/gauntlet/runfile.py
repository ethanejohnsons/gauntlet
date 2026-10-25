"""Read and write .run files: a recorded input sequence that both the sim and the
patched oDFe (gs_replay) can play back.

Text format, one frame per line after the header:

    gauntlet-run 1
    map vanilla_01
    delta <pitch> <yaw> <roll>      sim delta_angles when frame 0 is sent
    settle <n>                      leading frames that only settle the player
    frames <n>
    <angle0> <angle1> <angle2> <forward> <right> <up> <buttons> <weapon>
    ...

Angles are the raw 16-bit usercmd angles the sim was given. The engine adds the
difference between its own delta_angles and the recorded ones, so the view is the
same even though the real player spawned with a different mouse position.

The first `settle` frames just stand still: the engine player may have been
standing at the spawn for a while, the sim player has only just dropped in, and
these frames bring both to the same state. Comparisons start after them.
"""

from dataclasses import dataclass, field

from .sim import Cmd

MAGIC = "gauntlet-run 1"


@dataclass
class Run:
    map: str
    delta: tuple = (0, 0, 0)
    settle: int = 0
    frames: list = field(default_factory=list)   # (a0, a1, a2, forward, right, up, buttons, weapon)

    def add(self, cmd):
        self.frames.append((cmd.angles[0] & 65535, cmd.angles[1] & 65535, cmd.angles[2] & 65535,
                            cmd.forward, cmd.right, cmd.up, cmd.buttons, cmd.weapon))

    def cmds(self):
        for frame in self.frames:
            a0, a1, a2, f, r, u, b = frame[:7]
            c = Cmd()
            c.angles[0], c.angles[1], c.angles[2] = a0, a1, a2
            c.forward, c.right, c.up, c.buttons = f, r, u, b
            c.weapon = frame[7] if len(frame) > 7 else 0    # older runs have no weapon column
            yield c


def write(path, run):
    with open(path, "w") as f:
        f.write(f"{MAGIC}\nmap {run.map}\ndelta {run.delta[0]} {run.delta[1]} {run.delta[2]}\n")
        f.write(f"settle {run.settle}\n")
        f.write(f"frames {len(run.frames)}\n")
        for fr in run.frames:
            f.write(" ".join(str(v) for v in fr) + "\n")


def read(path):
    with open(path) as f:
        lines = [l.strip() for l in f if l.strip()]
    if lines[0] != MAGIC:
        raise ValueError(f"{path}: not a {MAGIC} file")
    header, i = {}, 1
    while not lines[i][0].isdigit():
        key, *vals = lines[i].split()
        header[key] = vals
        i += 1
        if key == "frames":
            break
    run = Run(map=header["map"][0], delta=tuple(int(v) for v in header["delta"]),
              settle=int(header.get("settle", ["0"])[0]))
    n = int(header["frames"][0])
    run.frames = [tuple(int(v) for v in l.split()) for l in lines[i:i + n]]
    if len(run.frames) != n:
        raise ValueError(f"{path}: expected {n} frames, found {len(run.frames)}")
    return run
