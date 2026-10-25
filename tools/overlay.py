#!/usr/bin/env python3
"""Draw the network's thinking onto a rendered run, frame by frame.

Takes the video from tools/render_run.sh and the thinking from tools/play_agent.py
and writes a new video with a panel on the right:

  - the network: inputs, two hidden layers (the 24 busiest neurons of each) and the
    outputs, lit by their real activations, with the strongest real connections
  - its choices: how sure it is about each key and turn rate, the pick in amber
  - its value estimate ("how well is this going?") over the last few seconds
  - the keys it's holding

Every number drawn is what the network computed at that moment. The timing comes
from the engine: gs_video.log gives each video frame's game time, gs_replay.log the
game time of the run's first frame.

    .venv/bin/python tools/overlay.py build/video/tatmt-s2_ai.mp4 build/runs/tatmt-s2_ai.think.npz \\
        runs/tatmt-s2/second/agent.pt --label "15 MIN OF TRAINING"
"""

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
import torch
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from gauntlet import env as genv, sim  # noqa: E402

FONT_PATH = "/usr/share/fonts/TTF/MxPlus_IBM_VGA_9x16.ttf"   # int10h.org Oldschool PC fonts, CC BY-SA 4.0

# colours: dark panels, amber for picks and numbers, soft green / red for signs.
# Everything is opaque, so the panel looks the same over a bright sky or a dark hall.
PANEL = (28, 29, 32, 255)
EDGE_LIGHT, EDGE_DARK = (62, 63, 69, 255), (20, 21, 23, 255)
TEXT, DIM = (201, 199, 191), (123, 121, 115)
AMBER, GREEN, RED = (232, 176, 74), (127, 209, 139), (217, 118, 95)

CHOICES = [("FWD", ["BACK", "-", "FWD"]), ("SIDE", ["LEFT", "-", "RIGHT"]), ("JUMP", ["NO", "YES"]),
           ("TURN", [f"{r:+g}" for r in genv.TURN_RATES]), ("FIRE", ["NO", "YES"]),
           ("AIM", [f"{p:g}" for p in genv.PITCHES])]
SHOWN = 24      # hidden neurons drawn per layer


def read_log(path):
    """An engine log: its header lines as {key: [values]}, and the first two numbers of
    each data line (for gs_video.log: video frame, game time)."""
    header, rows = {}, []
    for line in open(path):
        parts = line.split()
        if parts and parts[0].isdigit():
            rows.append([int(float(v)) for v in parts[:2]])
        elif parts:
            header[parts[0]] = parts[1:]
    return header, np.array(rows)


def busiest(acts, k):
    """The k neurons whose activation changes most over the run (sorted by index)."""
    return np.sort(np.argsort(-acts.std(axis=0))[:k])


def mix(color, t):
    """color at strength t (0..1), faded into the panel colour (opaque)."""
    t = float(np.clip(t, 0, 1))
    return tuple(int(PANEL[i] + (color[i] - PANEL[i]) * t) for i in range(3)) + (255,)


def signed(v):
    return mix(GREEN if v >= 0 else RED, abs(v))


class Panel:
    """The panel: the network on top, its choices, its value estimate and the keys below.
    It sits on the right, between DeFRaG's third-person view (top right) and its timer
    (bottom right), and keeps text to the few labels needed to read it."""

    def __init__(self, think, agent, size, label):
        self.t = think
        self.w, self.h = size
        self.font = ImageFont.truetype(FONT_PATH, 16)
        self.label = label
        self.sel1 = busiest(think["hidden1"], SHOWN)
        self.sel2 = busiest(think["hidden2"], SHOWN)
        # the real weights between the neurons that are drawn
        self.w1 = agent["body.0.weight"].numpy()[self.sel1]                       # (24, inputs)
        self.w2 = agent["body.2.weight"].numpy()[np.ix_(self.sel2, self.sel1)]    # (24, 24)
        self.w3 = agent["policy.weight"].numpy()[:, self.sel2]                    # (19, 24)
        self.values = think["value"]

        # layout, scaled from a 1280x720 frame: right edge, y 150..650
        sx, sy = self.w / 1280, self.h / 720
        self.x0, self.y0 = int(948 * sx), int(150 * sy)
        self.pw, self.ph = int(324 * sx), int(500 * sy)
        top, bottom = self.y0 + 14, self.y0 + int(285 * sy)
        cols = [self.x0 + self.pw * f for f in (0.08, 0.37, 0.66, 0.92)]
        n_in = think["obs"].shape[1]
        self.inputs = [(cols[0], top + i * (bottom - top) / (n_in - 1)) for i in range(n_in)]
        self.h1 = [(cols[1], top + 6 + i * (bottom - top - 12) / (SHOWN - 1)) for i in range(SHOWN)]
        self.h2 = [(cols[2], top + 6 + i * (bottom - top - 12) / (SHOWN - 1)) for i in range(SHOWN)]
        # outputs: one column, a small gap between the four choices
        self.outputs, y = [], top
        step = (bottom - top) / ( sum(len(n) for _, n in CHOICES) + len(CHOICES) / 2 )
        for _, names in CHOICES:
            for _ in names:
                self.outputs.append((cols[3], y))
                y += step
            y += step / 2
        self.rows_y = bottom + 16
        self.background = self._background()

    def _box(self, d, box):
        x0, y0, x1, y1 = box
        d.rectangle(box, fill=PANEL)
        d.line([(x0, y1), (x0, y0), (x1, y0)], fill=EDGE_LIGHT)
        d.line([(x1, y0), (x1, y1), (x0, y1)], fill=EDGE_DARK)

    def _background(self):
        img = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        self._box(d, (self.x0, self.y0, self.x0 + self.pw, self.y0 + self.ph))
        y = self.rows_y
        for name, _ in CHOICES:
            d.text((self.x0 + 10, y), name, font=self.font, fill=DIM)
            y += 20
        # the "AI is playing" tag, top middle of the frame
        tag = f"AI PLAYING  ·  {self.label}" if self.label else "AI PLAYING"
        tw = d.textlength(tag, font=self.font)
        tx = (self.w - tw) / 2 - 10
        self._box(d, (tx, 8, tx + tw + 20, 34))
        d.text((tx + 10, 12), tag, font=self.font, fill=AMBER)
        return img

    def draw(self, d_index):
        img = self.background.copy()
        d = ImageDraw.Draw(img)
        t = self.t
        obs, h1 = t["obs"][d_index], t["hidden1"][d_index][self.sel1]
        h2, probs = t["hidden2"][d_index][self.sel2], t["probs"][d_index]
        action = t["action"][d_index]

        # connections: for each neuron, its strongest incoming weighted signals
        def links(src_pts, src_act, dst_pts, w, k=3):
            for j, (dx, dy) in enumerate(dst_pts):
                signal = w[j] * src_act
                for i in np.argsort(-np.abs(signal))[:k]:
                    strength = min(1.0, abs(signal[i]) / 1.5)
                    if strength > 0.05:
                        sx, sy = src_pts[i]
                        d.line([(sx, sy), (dx, dy)], fill=signed(np.sign(signal[i]) * (0.25 + 0.6 * strength)))
        links(self.inputs, obs, self.h1, self.w1)
        links(self.h1, h1, self.h2, self.w2)
        links(self.h2, h2, self.outputs, self.w3, k=2)

        # neurons: square pixels, lit by activation; outputs by probability, the pick in amber
        for (x, y), v in zip(self.inputs, obs):
            d.rectangle([x - 3, y - 3, x + 3, y + 3], fill=signed(np.clip(v, -1, 1)), outline=EDGE_LIGHT)
        for pts, acts in ((self.h1, h1), (self.h2, h2)):
            for (x, y), v in zip(pts, acts):
                d.rectangle([x - 4, y - 4, x + 4, y + 4], fill=signed(v), outline=EDGE_LIGHT)
        offset = 0
        for g, (_, names) in enumerate(CHOICES):
            for k in range(len(names)):
                x, y = self.outputs[offset + k]
                color = AMBER if k == action[g] else GREEN
                d.rectangle([x - 4, y - 4, x + 4, y + 4], fill=mix(color, probs[offset + k]), outline=EDGE_LIGHT)
            offset += len(names)

        # choices: one bar per choice, split by probability, the pick in amber
        x0, y = self.x0 + 60, self.rows_y
        bw = self.pw - 72
        offset = 0
        for g, (_, names) in enumerate(CHOICES):
            left = x0
            for k in range(len(names)):
                right = left + bw * float(probs[offset + k])
                if right - left >= 1:
                    shade = AMBER if k == action[g] else (GREEN if k % 2 == 0 else (88, 150, 98))
                    d.rectangle([left, y + 3, right - 1, y + 13], fill=shade)
                left = right
            offset += len(names)
            y += 20

        # value estimate over the last 4 seconds: how well the network thinks it's going
        y += 6
        hist = self.values[max(0, d_index - 125):d_index + 1]
        lo, hi = float(self.values.min()), float(self.values.max()) + 1e-6
        gx, gw, gh = self.x0 + 10, self.pw - 20, 22
        pts = [(gx + gw * i / 125, y + gh - gh * (v - lo) / (hi - lo)) for i, v in enumerate(hist)]
        if len(pts) > 1:
            d.line(pts, fill=GREEN, width=2)

        # the keys it's holding
        y += gh + 10
        keys = [("W", action[0] == 2), ("A", action[1] == 0), ("S", action[0] == 0), ("D", action[1] == 2),
                ("JUMP", action[2] == 1), ("FIRE", action[4] == 1)]
        kx = self.x0 + 10
        for k, on in keys:
            kw = 9 * len(k) + 12
            self._box(d, (kx, y, kx + kw, y + 22))
            d.text((kx + 6, y + 3), k, font=self.font, fill=AMBER if on else DIM)
            kx += kw + 6
        return img


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video", help="mp4 from tools/render_run.sh")
    ap.add_argument("think", help=".think.npz from tools/play_agent.py")
    ap.add_argument("agent", help="the agent.pt that made the run (for its connection weights)")
    ap.add_argument("--label", default="", help="text after AI PLAYING in the corner tag")
    ap.add_argument("--start", type=float, default=0, help="start the output this many seconds in")
    ap.add_argument("--end", type=float, default=None, help="end the output at this many seconds")
    ap.add_argument("-o", "--out", help="output mp4 (default: <video>_overlay.mp4)")
    args = ap.parse_args()

    video = Path(args.video)
    _, frames = read_log(video.with_suffix(".video.log"))
    replay_header, _ = read_log(video.with_suffix(".replay.log"))
    base = int(replay_header["base"][0])          # game time of the run's frame 0
    think = dict(np.load(args.think))
    agent = torch.load(args.agent, map_location="cpu")

    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=width,height,r_frame_rate", "-of", "csv=p=0", str(video)],
                           capture_output=True, text=True, check=True).stdout.strip().split(",")
    w, h, fps = int(probe[0]), int(probe[1]), probe[2]
    panel = Panel(think, agent, (w, h), args.label.upper())

    out = Path(args.out) if args.out else video.with_name(video.stem + "_overlay.mp4")
    dec = subprocess.Popen(["ffmpeg", "-v", "error", "-i", str(video), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                           stdout=subprocess.PIPE)
    enc = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{w}x{h}",
                            "-r", fps, "-i", "-", "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p",
                            "-movflags", "+faststart", str(out)], stdin=subprocess.PIPE)
    rate = float(fps.split("/")[0]) / float(fps.split("/")[1]) if "/" in fps else float(fps)
    first, last = int(args.start * rate), int(args.end * rate) if args.end else None
    k, written = 0, 0
    while True:
        raw = dec.stdout.read(w * h * 3)
        if len(raw) < w * h * 3 or (last is not None and k >= last):
            break
        if k < first:
            k += 1
            continue
        # which decision is on screen: game time -> run frame -> last decision started by then
        game_time = frames[min(k, len(frames) - 1), 1]
        run_frame = (game_time - base) // sim.FRAME_MSEC
        d_index = int(np.searchsorted(think["frame"], run_frame, side="right")) - 1
        img = Image.frombytes("RGB", (w, h), raw).convert("RGBA")
        if d_index >= 0:
            img.alpha_composite(panel.draw(min(d_index, len(think["frame"]) - 1)))
        else:
            img.alpha_composite(panel.background)
        enc.stdin.write(img.convert("RGB").tobytes())
        k += 1
        written += 1
    enc.stdin.close()
    enc.wait()
    dec.kill()
    print(f"wrote {out} ({written} frames)")


if __name__ == "__main__":
    main()
