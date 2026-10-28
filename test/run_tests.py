# Scenario tests for the drum kit core. Usage: python3 -I run_tests.py <core.so>
import subprocess, sys, os, struct, tempfile, math
CORE = os.path.abspath(sys.argv[1]); HERE = os.path.dirname(os.path.abspath(__file__))
FR = 735; SR = 44100
fails = 0
def ok(c, m):
    global fails
    print(("PASS " if c else "FAIL ") + m); fails += (not c)

def run(lines, keep=False):
    d = tempfile.mkdtemp(prefix="dk-")
    open(os.path.join(d, "s.txt"), "w").write("\n".join(lines) + "\n")
    subprocess.run([os.path.join(HERE, "..", "build", "front"), CORE, os.path.join(d, "s.txt"), d], check=True, capture_output=True)
    raw = open(os.path.join(d, "audio.raw"), "rb").read()
    mono = struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]
    return d, mono

def press(f, b, n=2):  # hold button b for n frames starting at frame f
    return [f"{f+i} {b}" for i in range(n)]

def onsets(x, thr=6000, gap=0.04):
    out, last = [], -1e9
    for i, v in enumerate(x):
        if abs(v) > thr and i - last > gap * SR:
            out.append(i / SR); last = i
        if abs(v) > thr: last = i
    return out

# 1. Pads make sound immediately, every instrument
lines = ["2 START"]  # metronome off (not playing anyway)
btns = ["B","A","Y","X","UP","LEFT","DOWN","RIGHT","L2","L3"]
for k, b in enumerate(btns): lines += press(20 + k * 150, b)
lines.append(f"{20 + len(btns)*150 + 60} -")
d, x = run(lines)
lat, peaks = [], []
for k in range(10):
    t = (20 + k * 150) * FR
    seg = x[t:t + int(0.1 * SR)]
    peaks.append(max(abs(v) for v in seg))
    first = next((i for i, v in enumerate(seg) if abs(v) > 2000), None)
    lat.append(None if first is None else round(first / FR, 2))
ok(all(p > 4000 for p in peaks), f"10 instruments all sound: peaks {peaks}")
ok(all(l is not None and l < 1.0 for l in lat), f"pad latency within the same frame (frames): {lat}")

# 2. Loop record: R1 tap -> records + plays; kick on every beat for 2 bars; R1 tap -> stop recording; loop repeats
B = 36  # frames per beat at 100 BPM
lines = ["2 START"]                       # metronome off for clean onsets
lines += press(10, "R", 3)                 # released at frame 13 -> recording starts in frame 13
start = 13
for k in range(8): lines += press(start + k * B + 1, "B", 2)   # hits ~1 frame after each beat
lines += press(start + 8 * B + 10, "R", 3)    # stop recording
lines.append(f"{start + 8 * B * 3} -")       # run 3 loops total
d, x = run(lines)
on = onsets(x, thr=6000, gap=0.3)
t0 = start / 60
rel = [round((o - t0) / 0.6, 3) for o in on]
loop2 = [r for r in rel if 8 <= r < 16]; loop3 = [r for r in rel if 16 <= r < 24]
ok(len(loop2) == 8 and len(loop3) == 8, f"recorded 8 kicks replay in loop 2 and 3: {len(loop2)}, {len(loop3)}")
ok(all(abs(r - round(r)) < 0.03 for r in loop2 + loop3), f"replayed kicks land on the beat (quantized): max err {max(abs(r-round(r)) for r in loop2+loop3):.3f} beat")
first = [r for r in rel if r < 8]
ok(len(first) == 8, f"no double hits while recording (snapped-forward hits skipped once): {len(first)} in loop 1")

# 3. Hold R1 = clear
lines += press(start + 8 * B * 3 + 5, "R", 40)
lines.append(f"{start + 8 * B * 5} -")
d, x = run(lines)
after = [o for o in onsets(x, thr=6000, gap=0.3) if o > (start + 8 * B * 3 + 50) / 60]
ok(len(after) == 0, f"hold R1 clears the loop: {len(after)} hits after clearing")

# 4. L1 play/stop with metronome: 4 clicks per bar, first louder
lines = []
lines += press(10, "L", 2)
lines += press(10 + 8 * B - 10, "L", 2)
lines.append(f"{10 + 8 * B + 60} -")
d, x = run(lines)
on = onsets(x, thr=3000, gap=0.3)
ok(len(on) == 8, f"metronome: 8 clicks in 2 bars then stop: {len(on)}")
iv = [round((b - a) * 60, 1) for a, b in zip(on, on[1:])]
ok(all(abs(v - 36) <= 1 for v in iv), f"click interval 36 frames (100 BPM): {iv}")

# 5. Sequencer: R2, A on step 1 (kick row), move right 4 -> A ... 8 kicks on beats, play with L1
lines = ["2 START"]
lines += press(5, "R2", 2)                # sequencer
f = 10
for k in range(8):
    lines += press(f, "A", 2); f += 4
    for _ in range(4): lines += press(f, "RIGHT", 1); f += 3
lines += press(f + 5, "L", 2)
pstart = f + 5
lines.append(f"{pstart + 8 * B * 2} -")
d, x = run(lines)
on = [o for o in onsets(x, thr=6000, gap=0.3) if o > (pstart - 1) / 60]
rel = [round((o - pstart / 60) / 0.6, 2) for o in on]
ok(len(on) == 16 and all(abs(r - round(r)) < 0.03 for r in rel), f"sequencer steps play on every beat for 2 loops: {len(on)} hits {rel[:5]}...")

# 6. Sequencer A again removes the step; X/Y change tempo; save + reload keeps the pattern
lines = ["2 START"] + press(5, "R2", 2) + press(10, "A", 2) + press(20, "A", 2)   # on then off
for k in range(20): lines += press(30 + k * 3, "X", 1)                          # +20 BPM
lines += press(100, "A", 2)                                                     # step 1 on
lines.append("200 -")
d, x = run(lines, keep=True)
dat = open(os.path.join(d, "ours-blanc-drumkit.dat"), "rb").read()
bpm = struct.unpack("<H", dat[6:8])[0]; n = struct.unpack("<H", dat[9:11])[0]
ok(bpm == 120 and n == 1, f"saved: bpm {bpm} (expect 120), events {n} (expect 1)")

print("ALL PASSED" if not fails else f"{fails} FAILED"); sys.exit(1 if fails else 0)
