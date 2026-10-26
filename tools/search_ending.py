#!/usr/bin/env python3
"""Search for a better ending to a run: from a moment of it, try thousands of variations
of the rest and keep the best (a finish first, then the fastest).

The sim is deterministic, so a run can be resumed exactly at any decision. From there
an evolutionary search plays a population of action sequences in parallel: each
generation, every sequence is a copy of one of the best so far with a few decisions
changed at random (single choices, or a short stretch at once). Sequences are scored
by whether they finish and how fast, or else by how far along the route they got
while alive.

This is a search over inputs, not the network playing: use what it finds as a seed
(train/ppo.py --seed-runs) so the network learns to do it itself. The output is a
whole run from the spawn (the original run up to the moment, then the found ending),
so tools/render_run.sh can check it in the real game.

    .venv/bin/python tools/search_ending.py runs/tatmt-s2/finish10/best_progress.run \\
        --back 2 4 6 -o build/runs/tatmt-s2_found
"""

import argparse
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from gauntlet import env as genv, runfile, sim  # noqa: E402

DECISION_SECONDS = genv.FRAMES_PER_ACTION * sim.FRAME_MSEC / 1000


def recover_actions(run, max_seconds):
    """The run's decisions, recovered from its inputs, and a player at its start."""
    player = genv.VecEnv(run.map, 1, max_seconds=max_seconds, threads=1, stuck_seconds=None)
    player.version = -1
    player.reset()
    frames = np.array(run.frames[genv.SETTLE_FRAMES:], np.int64)
    actions = []
    for k in range(len(frames) // genv.FRAMES_PER_ACTION):
        action = player._action_for(frames[k * genv.FRAMES_PER_ACTION:(k + 1) * genv.FRAMES_PER_ACTION])
        if action is None:
            sys.exit(f"decision {k} isn't one of the env's actions")
        actions.append(action)
        if player.step(action[None])[2][0]:
            break
    return np.array(actions), player


def moment_at(run, actions, decision, max_seconds):
    """The run resumed at a decision, as a Restart."""
    player = genv.VecEnv(run.map, 1, max_seconds=max_seconds, threads=1, stuck_seconds=None)
    player.version = -1
    player.reset()
    for a in actions[:decision]:
        player.step(a[None])
    return player.moment(0)


def mutate(parents, rng, rate):
    """Children of the parents: each decision's choices re-drawn with probability `rate`,
    and in half of them a short stretch (1-12 decisions) re-drawn whole."""
    children = parents.copy()
    n, horizon, heads = children.shape
    sizes = np.array(genv.ACTION_SIZES)
    draw = rng.random((n, horizon, heads)) < rate
    children[draw] = (rng.random(draw.sum()) * np.broadcast_to(sizes, draw.shape)[draw]).astype(np.int64)
    for j in np.nonzero(rng.random(n) < 0.5)[0]:
        length = rng.integers(1, 13)
        at = rng.integers(0, max(1, horizon - length))
        children[j, at:at + length] = (rng.random((length, heads)) * sizes).astype(np.int64)
    return children


def evaluate(env, start, sequences):
    """Play each sequence from `start`; return (finished, timer_ms, furthest progress,
    decisions played, the run and decisions of each that finished)."""
    n, horizon, _ = sequences.shape
    env.start_all_from(start)
    alive = np.ones(n, bool)
    finished = np.zeros(n, bool)
    timer = np.zeros(n, np.int64)
    furthest = np.zeros(n)
    played = np.full(n, horizon)
    runs = {}
    for t in range(horizon):
        _, _, done, info = env.step(sequences[:, t])
        progress = env._progress(env._leg_route(env.route, env.leg), env.leg)
        furthest = np.where(alive & (env.states["dead"] == 0), np.maximum(furthest, progress), furthest)
        for k, i in enumerate(info.get("ended", [])):
            if alive[i]:
                alive[i] = False
                played[i] = t + 1
                if info["finished"][k]:
                    finished[i], timer[i], furthest[i] = True, info["timer_ms"][k], 1.0
                    runs[i] = (info["runs"][i], info["decisions"][i])
        if not alive.any():
            break
    return finished, timer, furthest, played, runs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", help="a .run that gets near the end")
    ap.add_argument("-o", "--out", required=True, help="output path, without extension")
    ap.add_argument("--back", type=float, nargs="+", default=[2, 4, 6],
                    help="seconds before the run's end to search from, tried in turn")
    ap.add_argument("--horizon", type=float, default=10, help="seconds of ending to search")
    ap.add_argument("--population", type=int, default=2048)
    ap.add_argument("--generations", type=int, default=150)
    ap.add_argument("--keep", type=int, default=64, help="best sequences kept each generation")
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    run = runfile.read(args.run)
    max_seconds = 120
    actions, _ = recover_actions(run, max_seconds)
    print(f"{args.run}: {len(actions)} decisions ({len(actions) * DECISION_SECONDS:.1f} s)")
    env = genv.VecEnv(run.map, args.population, max_seconds=max_seconds, threads=args.threads, stuck_seconds=None)
    horizon = int(args.horizon / DECISION_SECONDS)

    for back in args.back:
        start_at = max(0, len(actions) - int(back / DECISION_SECONDS))
        start = moment_at(run, actions, start_at, max_seconds)
        # the population starts as the run's own ending (padded with random decisions)
        base = (rng.random((horizon, len(genv.ACTION_SIZES))) * np.array(genv.ACTION_SIZES)).astype(np.int64)
        tail = actions[start_at:start_at + horizon]
        base[:len(tail)] = tail
        elite = np.repeat(base[None], args.keep, axis=0)
        best = None
        t0 = time.time()
        print(f"searching from decision {start_at} ({back:g} s before the end)")
        for gen in range(args.generations):
            children = mutate(elite[np.arange(args.population) % len(elite)], rng, rate=0.03)
            children[:len(elite)] = elite                       # the elite play again, unchanged
            finished, timer, furthest, played, runs = evaluate(env, start, children)
            score = np.where(finished, 2 + (100000 - timer) / 100000, furthest)
            order = np.argsort(-score)
            elite = children[order[:args.keep]]
            top = order[0]
            if finished[top] and (best is None or timer[top] < best[0]):
                best = (int(timer[top]), runs[top])
            if gen % 10 == 0 or finished.any():
                print(f"  generation {gen:3d}: best progress {furthest.max():.4f}, finishes {finished.sum()}"
                      + (f", fastest {timer[finished].min() / 1000:.3f} s" if finished.any() else "")
                      + f"  ({time.time() - t0:.0f} s)", flush=True)
            if best is not None and gen >= 10 and finished.sum() > args.population // 4:
                break                                             # settled on finishing
        if best is not None:
            timer_ms, (frames, (acts, versions)) = best
            out = Path(args.out)
            out.parent.mkdir(parents=True, exist_ok=True)
            runfile.write(out.with_suffix(".run"), env.make_run(frames))
            np.savez_compressed(out.with_suffix(".decisions.npz"), action=acts, version=versions)
            print(f"found a finish in {timer_ms / 1000:.3f} s: wrote {out.with_suffix('.run')}")
            return
        print(f"no finish from {back:g} s before the end")
    sys.exit("no finish found")


if __name__ == "__main__":
    main()
