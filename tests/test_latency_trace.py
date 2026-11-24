"""tools/latency_trace.py on made-up traces: a picture's age and frames behind come from the game frame that drew it,
so a pipeline one frame deeper reads one frame (16.7 ms) older, and a press is matched to the first present whose
picture is from its frame plus the game's part."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import latency_trace  # noqa: E402

F = 1000.0 / 60


def make_trace(path, depth, presses=(), frames=400, status_after=None):
    """frame k: refresh at k*F, pad read 0.5 ms later, queued 3 ms later; the picture of frame k - depth is presented
    at the refresh of frame k + 1"""
    lines = []
    for k in range(1, frames):
        t = k * F
        lines.append(f"R {t:.3f}")
        lines.append(f"I {t + 0.5:.3f} {k}")
        if k in presses:
            lines.append(f"P {t + 0.6:.3f} {k} {k} 10 {k}")
            lines.append(f"P {t + 0.7 + 4 * F:.3f} {k} {k} 0 {k + 4}")
        if status_after is not None and k - status_after in presses:
            lines.append(f"C {t + 1:.3f} {k} 5 abc")
        lines.append(f"Q {t + 3:.3f} {k} 1")
        pic = k - depth
        if pic > 0:
            p = (k + 1) * F + 0.2
            lines.append(f"S {p:.3f} {p + 0.3:.3f} {k} {k} {k + 1} {pic} 1")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def test_one_frame_deeper_reads_one_frame_older(tmp_path):
    a, b = tmp_path / "a.txt", tmp_path / "b.txt"
    make_trace(a, 0, presses=(100, 200))
    make_trace(b, 1, presses=(100, 200))
    ra = latency_trace.analyse(latency_trace.read_trace(str(a)), game=2)
    rb = latency_trace.analyse(latency_trace.read_trace(str(b)), game=2)
    assert abs(rb["age_ms"]["median"] - ra["age_ms"]["median"] - F) < 0.01
    assert ra["behind_frames"]["median"] == 1 and rb["behind_frames"]["median"] == 2
    assert [p["tick"] for p in ra["presses"]] == [100, 200]
    assert abs(rb["press_ms"]["median"] - ra["press_ms"]["median"] - F) < 0.01
    # the press read at frame 100 + the game's 2 frames: frame 102's picture, presented at the refresh of 103
    assert abs(ra["presses"][0]["ms"] - (103 * F + 0.2 - (100 * F + 0.6))) < 0.01


def test_no_fight_measures_the_whole_run(tmp_path):
    a = tmp_path / "a.txt"
    make_trace(a, 0, frames=900)
    t = latency_trace.read_trace(str(a))
    t["Q"] = [(ms, k, 0) for ms, k, _ in t["Q"]]
    r = latency_trace.analyse(t)
    assert "note" in r and r["age_ms"]["n"] > 0


def test_status_lines_give_the_games_part(tmp_path):
    a, b = tmp_path / "a.txt", tmp_path / "b.txt"
    make_trace(a, 0, presses=(100, 200), status_after=2)
    make_trace(b, 0, presses=(100, 200), status_after=1)   # (run-ahead: the status changes a frame sooner)
    ra = latency_trace.analyse(latency_trace.read_trace(str(a)))
    rb = latency_trace.analyse(latency_trace.read_trace(str(b)))
    assert ra["game_frames"]["median"] == 2 and rb["game_frames"]["median"] == 1
    assert abs(ra["press_ms"]["median"] - rb["press_ms"]["median"] - F) < 0.01
