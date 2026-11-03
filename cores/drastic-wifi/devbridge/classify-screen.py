#!/usr/bin/env python3
"""Classify an Asphalt screenshot against cropped, known final states."""

import argparse
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def distance(candidate: Image.Image, baseline: Image.Image, crop: list[int]) -> float:
    box = tuple(crop)
    left = candidate.convert("L").crop(box)
    right = baseline.convert("L").crop(box)
    if left.size != right.size:
        right = right.resize(left.size)
    return ImageStat.Stat(ImageChops.difference(left, right)).mean[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=("host", "client"))
    parser.add_argument("image", type=Path)
    parser.add_argument("--baselines", type=Path,
                        default=Path(__file__).with_name("asphalt-screen-baselines.json"))
    parser.add_argument("--max-distance", type=float, default=18.0)
    parser.add_argument("--min-margin", type=float, default=3.0)
    args = parser.parse_args()

    definitions = json.loads(args.baselines.read_text())[args.role]
    candidate = Image.open(args.image)
    results = []
    for definition in definitions:
        baseline_path = (args.baselines.parent / definition["image"]).resolve()
        baseline = Image.open(baseline_path)
        results.append((distance(candidate, baseline, definition["crop"]),
                        definition["state"]))
    results.sort()

    best_distance, best_state = results[0]
    margin = results[1][0] - best_distance if len(results) > 1 else 255.0
    confident = best_distance <= args.max_distance and margin >= args.min_margin
    result = {
        "classification": best_state if confident else "ambiguous",
        "best_state": best_state,
        "distance": round(best_distance, 3),
        "margin": round(margin, 3),
        "scores": {state: round(score, 3) for score, state in results},
    }
    print(json.dumps(result, sort_keys=True))
    return 0 if confident else 2


if __name__ == "__main__":
    raise SystemExit(main())
