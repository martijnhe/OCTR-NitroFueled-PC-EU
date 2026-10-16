#!/usr/bin/env bash
# Rough Steam Deck simulation on a desktop PC, to find what the Deck would struggle with.
#
#   tools/deck_sim.sh [--native] [--race] [--seconds N] [GAME_DIR]
#   tools/deck_sim.sh --restore       # undo a DECK_CPU_MHZ clock cap left by an interrupted run
#
# Run it from a normal terminal: a whole test takes 3-4 minutes.
#
# Runs the game (run.sh) inside gamescope at the Deck's 1280x800, on 4 cores / 8 threads with a
# CPU-time cap (DECK_CPU_QUOTA, default 340%: four desktop Zen 5 cores at ~42%, about a Deck's
# four Zen 2 cores at 3.5 GHz) and a 14 GiB memory cap (the Deck's 16 GiB shared with the GPU),
# with Deck-like settings (60 FPS patch, no motion blur, no ultrawide, fullscreen). --race drives
# the controller into an Arcade race; --native runs the same test without the limits (a
# baseline). At the end: average FPS, slow frames and peak memory from the frame statistics.
#
# Not simulated: the Deck's GPU (8 RDNA 2 compute units, ~1.6 TFLOPS); a desktop GPU cannot be
# slowed to it, so GPU-heavy settings (4K, upscalers) still need a real Deck. The CPU cap limits
# the total, not each thread: one busy thread can still run faster than on a Deck.
# DECK_CPUS (default 0-3,6-9: four cores with both of their threads) picks the cores; check
# /sys/devices/system/cpu/cpu*/topology/thread_siblings_list on other CPUs.
# DECK_CPU_MHZ=2400 also caps those cores' clock (sudo, cpupower; restored at the end): a modern
# desktop core at ~2.4 GHz is about a Deck core at 3.5 GHz. Without it each thread runs at desktop
# speed, which hides the Deck's real limit (the GPU command thread: 37-45 FPS in races on a Deck).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")/.." && pwd)
native=0 race=0 seconds=120 game=
while (( $# )); do
    case $1 in
        --restore)
            max=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq)
            sudo cpupower -c "${DECK_CPUS:-0-3,6-9}" frequency-set -u "$((max / 1000))MHz" >/dev/null
            echo "CPU clock of ${DECK_CPUS:-0-3,6-9} restored to $((max / 1000)) MHz"
            exit 0 ;;
        --native) native=1 ;;
        --race) race=1 ;;
        --seconds) seconds=$2; shift ;;
        *) game=$1 ;;
    esac
    shift
done
if [[ -z $game ]]; then
    saved=${XDG_CONFIG_HOME:-$HOME/.config}/sacredfire/game_dir
    [[ -f $saved ]] && game=$(<"$saved")
fi
[[ -n $game && -f $game/eboot.bin ]] || { echo "Usage: $0 [--native] [--race] [--seconds N] GAME_DIR" >&2; exit 1; }
if pgrep -x sacredfire >/dev/null; then echo "The game is already running: close it first." >&2; exit 1; fi

mode=deck; (( native )) && mode=native
logs=$here/logs; mkdir -p "$logs"
stamp=$(date +%Y%m%d_%H%M%S)
log=$logs/deck_sim_${mode}_$stamp.log
frames=$logs/deck_sim_${mode}_$stamp.frames.csv
config=$here/out/deck_sim.ini
mkdir -p "$here/out"
printf '%s\n' patch_60fps=1 patch_no_motion_blur=1 ultrawide=0 ultrawide_ask=0 show_fps=1 fullscreen=1 \
    > "$config"

game_command=(env BB_CONFIG="$config" BB_GAME_DIR="$game" BB_FULLSCREEN=1 BB_TIMEOUT="$seconds"
    BB_FRAME_STATS=1 BB_FRAME_LOG="$frames" bash "$here/run.sh")
if (( native )); then
    command=(gamescope -W 1280 -H 800 -w 1280 -h 800 -r 60 -- "${game_command[@]}")
else
    # With DECK_CPU_MHZ the slower clock stands for the Deck's CPU; a CPU-time quota on top froze
    # the game whenever its share ran out (60-70 ms frames a Deck does not have).
    quota=${DECK_CPU_QUOTA:-340%}
    [[ -n ${DECK_CPU_MHZ:-} && -z ${DECK_CPU_QUOTA:-} ]] && quota=infinity
    command=(systemd-run --user --scope --quiet -p MemoryMax=14G -p CPUQuota="$quota"
        taskset -c "${DECK_CPUS:-0-3,6-9}"
        gamescope -W 1280 -H 800 -w 1280 -h 800 -r 60 -- "${game_command[@]}")
fi
# Build first (run.sh would otherwise build inside the timed, capped run).
(cd "$here" && bash build.sh > "$logs/deck_sim_build.log" 2>&1) || { echo "Build failed: $logs/deck_sim_build.log" >&2; exit 1; }
if (( ! native )) && [[ -n ${DECK_CPU_MHZ:-} ]]; then
    cpus=${DECK_CPUS:-0-3,6-9}
    max=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq)
    sudo cpupower -c "$cpus" frequency-set -u "${DECK_CPU_MHZ}MHz" >/dev/null
    restore() { sudo cpupower -c "$cpus" frequency-set -u "$((max / 1000))MHz" >/dev/null || true; }
    trap restore EXIT
    trap 'restore; pkill -x sacredfire; exit 130' INT TERM HUP
    echo "CPU clock of $cpus capped at ${DECK_CPU_MHZ} MHz (restored at the end; if a run is killed: $0 --restore)"
fi
echo "Steam Deck simulation ($mode): $log"
cd "$here"
"${command[@]}" > "$log" 2>&1 &
runner=$!

if (( race )); then
    # From the start: intros and legal screens, main menu, Local Arcade, a single race.
    for _ in $(seq 60); do pgrep -x sacredfire >/dev/null && break; sleep 1; done
    sleep 25
    for _ in $(seq 8); do bash tools/press.sh cross; sleep 4; done
    bash tools/press.sh down; sleep 1.5
    for _ in $(seq 4); do bash tools/press.sh cross; sleep 5; done
fi
peak=0
while kill -0 "$runner" 2>/dev/null; do
    pid=$(pgrep -x sacredfire | head -1 || true)
    if [[ -n $pid ]]; then
        rss=$(awk '/VmRSS/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
        (( rss > peak )) && peak=$rss
    fi
    sleep 2
done
wait "$runner" || true

python3 - "$frames" "$log" "$peak" "$mode" <<'EOF'
import csv, sys
frames, log, peak, mode = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
try:
    rows = [r for r in csv.DictReader(open(frames))
            if r.get('frame_ms') and r.get('draws') and float(r['frame_ms']) > 0]
except OSError:
    rows = []
rows = rows[len(rows) // 4:]  # skip loading and the intros
if not rows:
    print('No frame statistics (did the game start?): see', log)
    sys.exit()
ms = sorted(float(r['frame_ms']) for r in rows)
avg = sum(ms) / len(ms)
p99 = ms[int(len(ms) * 0.99) - 1]
slow = sum(1 for m in ms if m > 33.4)
draws = sum(float(r['draws']) for r in rows) / len(rows)
print(f'{mode}: {len(ms)} frames, average {1000 / avg:.1f} FPS ({avg:.1f} ms), 99% of frames under '
      f'{p99:.1f} ms, {slow} frames slower than 30 FPS ({100 * slow / len(ms):.1f}%), '
      f'{draws:.0f} draws/frame, peak memory {peak / 1048576:.1f} GiB')
EOF
