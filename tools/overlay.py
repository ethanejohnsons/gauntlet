#!/usr/bin/env python3
"""Draw the network's thinking onto a rendered run, frame by frame.

Takes the video from tools/render_run.sh and the thinking from tools/play_agent.py
(or tools/think.py) and writes a new video with a panel on the right:

  - the network: inputs, two hidden layers (the 24 busiest neurons of each) and the
    outputs, lit by their real activations, with the strongest real connections
  - its choices: how sure it is about each key and turn rate, the pick highlighted
  - its value estimate ("how well is this going?"), with its recent history
  - the keys it's holding and its speed

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

# colours: dark panels, amber for picks and numbers, soft green / red for signs
PANEL = (28, 29, 32, 240)
EDGE_LIGHT, EDGE_DARK = (62, 63, 69, 255), (20, 21, 23, 255)
TEXT, DIM = (201, 199, 191), (123, 121, 115)
AMBER, GREEN, RED = (232, 176, 74), (127, 209, 139), (217, 118, 95)

INPUT_GROUPS = [("MOTION", 0, 7), ("FEELERS", 7, 33), ("ROUTE", 33, 45), ("TIMER", 45, 46)]
CHOICES = [("FWD", ["BACK", "-", "FWD"]), ("SIDE", ["LEFT", "-", "RIGHT"]), ("JUMP", ["NO", "YES"]),
           ("TURN", [f"{r:+g}" for r in genv.TURN_RATES])]
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


def mix(color, t, alpha=255):
    """color at strength t (0..1), over the panel colour."""
    t = float(np.clip(t, 0, 1))
    return tuple(int(PANEL[i] + (color[i] - PANEL[i]) * t) for i in range(3)) + (alpha,)


def signed(v, alpha=255):
    return mix(GREEN if v >= 0 else RED, abs(v), alpha)


class Panel:
    def __init__(self, think, agent, size, label):
        self.t = think
        self.w, self.h = size
        self.font = ImageFont.truetype(FONT_PATH, 16)
        self.label = label
        self.sel1 = busiest(think["hidden1"], SHOWN)
        self.sel2 = busiest(think["hidden2"], SHOWN)
        # the real weights between the neurons that are drawn
        self.w1 = agent["body.0.weight"].numpy()[self.sel1]                       # (24, 46)
        self.w2 = agent["body.2.weight"].numpy()[np.ix_(self.sel2, self.sel1)]    # (24, 24)
        self.w3 = agent["policy.weight"].numpy()[:, self.sel2]                    # (19, 24)
        self.values = think["value"]

        # layout of the panel (x0, y0 = its top-left corner on the video)
        self.pw, self.ph = 372, self.h - 16
        self.x0, self.y0 = self.w - self.pw - 8, 8
        top, bottom = self.y0 + 34, self.y0 + 470
        cols = [self.x0 + 70, self.x0 + 150, self.x0 + 225, self.x0 + 300]
        self.inputs = [(cols[0], top + i * (bottom - top) / 45) for i in range(46)]
        self.h1 = [(cols[1], top + 8 + i * (bottom - top - 16) / (SHOWN - 1)) for i in range(SHOWN)]
        self.h2 = [(cols[2], top + 8 + i * (bottom - top - 16) / (SHOWN - 1)) for i in range(SHOWN)]
        self.outputs = []
        y = top
        for _, names in CHOICES:
            for _ in names:
                self.outputs.append((cols[3], y))
                y += (bottom - top) / 21
            y += (bottom - top) / 42
        self.background = self._background()

    def _box(self, d, box, title=None):
        x0, y0, x1, y1 = box
        d.rectangle(box, fill=PANEL)
        d.line([(x0, y1), (x0, y0), (x1, y0)], fill=EDGE_LIGHT)
        d.line([(x1, y0), (x1, y1), (x0, y1)], fill=EDGE_DARK)
        if title:
            d.text((x0 + 8, y0 + 4), title, font=self.font, fill=DIM)

    def _background(self):
        img = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        x0, y0 = self.x0, self.y0
        self._box(d, (x0, y0, x0 + self.pw, y0 + self.ph))
        d.text((x0 + 10, y0 + 6), "NEURAL NET", font=self.font, fill=DIM)
        d.text((x0 + self.pw - 10 - 9 * 10, y0 + 6), "LIVE INPUT", font=self.font, fill=DIM)
        for name, a, b in INPUT_GROUPS:
            ys = [self.inputs[i][1] for i in range(a, b)]
            d.text((x0 + 8, (ys[0] + ys[-1]) / 2 - 8), name[:7], font=self.font, fill=DIM)
        names = [n for _, ns in CHOICES for n in ns]
        for (x, y), n in zip(self.outputs, names):
            d.text((x + 9, y - 8), n, font=self.font, fill=DIM)
        # the "AI is playing" tag, top middle of the game view
        tag = f"AI PLAYING  ·  {self.label}" if self.label else "AI PLAYING"
        tw = d.textlength(tag, font=self.font)
        tx = (self.x0 - tw) / 2 - 10
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

        # connections: for each neuron, its 3 strongest incoming weighted signals
        def links(src_pts, src_act, dst_pts, w, k=3):
            for j, (dx, dy) in enumerate(dst_pts):
                signal = w[j] * src_act
                for i in np.argsort(-np.abs(signal))[:k]:
                    s = signal[i]
                    strength = min(1.0, abs(s) / 1.5)
                    if strength > 0.05:
                        sx, sy = src_pts[i]
                        d.line([(sx, sy), (dx, dy)], fill=signed(np.sign(s) * strength, int(60 + 150 * strength)))
        links(self.inputs, obs, self.h1, self.w1)
        links(self.h1, h1, self.h2, self.w2)
        links(self.h2, h2, self.outputs, self.w3, k=2)

        # neurons: square pixels, lit by activation
        for (x, y), v in zip(self.inputs, obs):
            d.rectangle([x - 3, y - 3, x + 3, y + 3], fill=signed(np.clip(v, -1, 1)), outline=EDGE_LIGHT)
        for pts, acts in ((self.h1, h1), (self.h2, h2)):
            for (x, y), v in zip(pts, acts):
                d.rectangle([x - 4, y - 4, x + 4, y + 4], fill=signed(v), outline=EDGE_LIGHT)
        offset = 0
        for g, (_, names) in enumerate(CHOICES):
            for k in range(len(names)):
                x, y = self.outputs[offset + k]
                p = probs[offset + k]
                chosen = k == action[g]
                d.rectangle([x - 4, y - 4, x + 4, y + 4], fill=mix(AMBER if chosen else GREEN, p), outline=EDGE_LIGHT)
            offset += len(names)

        # choices: one bar per choice, split by probability; the pick in amber
        x0, y = self.x0 + 10, self.y0 + 486
        d.text((x0, y), "CHOICES (HOW SURE)", font=self.font, fill=DIM)
        y += 22
        offset = 0
        bx, bw = x0 + 48, self.pw - 175
        for g, (name, names) in enumerate(CHOICES):
            d.text((x0, y), name, font=self.font, fill=DIM)
            left = bx
            for k in range(len(names)):
                p = float(probs[offset + k])
                right = left + bw * p
                if right - left >= 1:
                    shade = AMBER if k == action[g] else (GREEN if k % 2 == 0 else (88, 150, 98))
                    d.rectangle([left, y + 2, right - 1, y + 14], fill=shade)
                left = right
            pick = action[g]
            d.text((bx + bw + 8, y), f"{names[pick]:>5s} {probs[offset + pick]:3.0%}", font=self.font, fill=AMBER)
            offset += len(names)
            y += 20

        # value estimate, with the last 4 seconds of it
        y += 6
        v = float(t["value"][d_index])
        d.text((x0, y), "EXPECTS", font=self.font, fill=DIM)
        d.text((x0 + 80, y), f"{v:6.2f}", font=self.font, fill=AMBER)
        hist = self.values[max(0, d_index - 125):d_index + 1]
        lo, hi = float(self.values.min()), float(self.values.max()) + 1e-6
        gx, gw, gy, gh = x0 + 150, self.pw - 170, y + 2, 14
        pts = [(gx + gw * i / max(1, len(hist) - 1), gy + gh - gh * (h - lo) / (hi - lo)) for i, h in enumerate(hist)]
        if len(pts) > 1:
            d.line(pts, fill=GREEN, width=1)

        # keys and speed
        y += 26
        keys = [("W", action[0] == 2), ("A", action[1] == 0), ("S", action[0] == 0), ("D", action[1] == 2),
                ("JUMP", action[2] == 1)]
        kx = x0
        for k, on in keys:
            kw = 9 * len(k) + 12
            self._box(d, (kx, y, kx + kw, y + 22))
            d.text((kx + 6, y + 3), k, font=self.font, fill=AMBER if on else DIM)
            kx += kw + 6
        speed = float(obs[3]) * 800
        d.text((kx + 10, y + 3), f"{speed:4.0f} UPS", font=self.font, fill=GREEN)
        return img


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video", help="mp4 from tools/render_run.sh")
    ap.add_argument("think", help=".think.npz from tools/play_agent.py or tools/think.py")
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
