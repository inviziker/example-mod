# Predictive Input Buffer

A smarter input buffer. Instead of blindly delaying every click, it **simulates your trajectory** and only holds an input back when doing so actually keeps you alive.

## How it works

1. You click (or release in Wave / Robot).
2. The mod simulates your current trajectory with the click applied immediately.
3. If you survive, the click goes through untouched, with zero added delay.
4. If you would die, it searches for the **smallest delay** that clears the obstacle and fires the click at that moment.

Quick taps and releases during a delay are queued, so nothing gets eaten.

## Features

- **Predictive, not blind**: only delays when it saves you
- **Wave aware**: separate buffer strength for Wave and Mini Wave
- **Spam guard**: fast Wave spam bypasses the buffer so it stays crisp
- **Dual support**: both players are simulated together
- **Performance limiter**: a per-click time budget keeps your frame rate steady, even with huge buffers
- **No pause menu clutter**: everything lives in the mod's settings page

## Settings

Open **Geode → Mods → Predictive Input Buffer → Settings**.

| Setting | What it does |
|---|---|
| Buffer Strength | Max delay for non-Wave modes (1 tick = 1/240s) |
| Wave Buffer Strength | Max delay for Wave / Mini Wave |
| Spam Guard | Skips buffering during rapid Wave spam |
| Search Time Limit | Per-click CPU budget |
| Minimum Scan Step | Precision vs. speed for big buffers |
| Status Label | Optional on-screen counter |

**Tip:** 4 ticks = 1 frame at 60 FPS. The default of 2 ticks is half a frame.

## Notes

- Ship mode is never touched.
- Using this in rated or leaderboard-submitted runs may not be allowed by every list. Check the rules first.
