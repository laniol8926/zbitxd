# jt9 decoder-merge: live production results (2026-08-29)

First measurement of the jt9 decoder-merge bridge (`jt9_bridge.py` +
`jt9-bridge.service`) running live in production on the Pi, ~2 minutes
after going live as a systemd service. Not the offline prototype from
the night before (captured 20M audio, replayed through both decoders
after the fact) -- this is measured directly from the actual live
`zbitxd`/`jt9-bridge` journals during real, ongoing FT8 reception.

## Method

Pulled `journalctl -u zbitxd --since <bridge start time>` and counted:

- **Native**: our own decoder's raw `>> ...` decode lines.
- **jt9**: the bridge's own `FT8CONTINUE ...` deliveries (jt9's output,
  reformatted and fed back in over the existing remote-command TCP
  port -- see `jt9_bridge.py`'s own module comment).

Both sides deduped to `(slot_time, message_text)` sets before
comparing, so a message re-found across multiple SIC/OSD passes on our
own side only counts once, matching the actual set of distinct stations
heard, not raw decode-attempt volume.

## Results

| | Our own decoder | jt9 | **Merged (union)** |
|---|---|---|---|
| Total unique decodes | 148 | 183 | **191** |
| CQ calls | 41 | 50 | **52** |

**+29% total decode coverage (148 -> 191), +27% CQ coverage (41 -> 52)**
from merging the two decoders' output over the same real ~2-minute
window.

## The gain is genuinely bidirectional

Not just "jt9 is better" -- 43 messages appeared *only* via jt9 (real
signals our own decoder missed entirely), but 8 appeared *only* via our
own decoder (things jt9's own pipeline missed that our OSD/SIC passes
caught). 140 messages were caught by both, the expected large overlap
for strong/clean signals either implementation should decode. This
confirms the original hypothesis from the night before this was built:
the two decoders have different blind spots, so merging beats either
one alone rather than one simply subsuming the other.

## Consistency with the earlier offline prototype

Tracks the same directional pattern as the night-before offline test
(captured 20M audio: jt9 86/18 CQs, our own decoder 69/15 CQs, merged
union 102/21 CQs -- also a ~+20-30% gain) -- that test's prediction held
up under real, live production conditions, not just the one offline
sample it was based on.

## Caveats

- Small sample (~2 minutes, one band/time-of-day). A longer, multi-band,
  multi-time-of-day sample would give a more statistically solid
  percentage, though the *direction* (merging helps, bidirectionally) is
  already well-supported.
- Not yet accounting for de-duplication of a message decoded by *both*
  sides as it flows into the operator-facing UI (Band Activity/CQ
  Panel) -- the 140 "decoded by both" messages currently still surface
  as two separate arrivals there. That's the next real step in this
  feature, not yet built.
