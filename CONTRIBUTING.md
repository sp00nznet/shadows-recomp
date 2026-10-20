# Contributing

Issues and pull requests are welcome: a wall you hit, a level that breaks, a
fix.

## What helps most

- **A report with a log.** Run `build\rogue.exe --run --watchdog 120 2> run.log`
  and attach `run.log`; say what was on screen and what you pressed.
- **A headless reproduction** if you can make one: `--headless --record r.mp4`
  with `--key`, `--mouse` and `--click` (docs/host.md) shows the problem
  without anyone needing your machine.
- **One change per pull request**, with what was wrong, how it showed, and how
  you checked the fix.

## What can't come in

- **Nothing from the game.** No executables, data, saves, extracted assets or
  lifted C (`src/recomp/gen/`), not even in a test.
- **Toolkit fixes go to [pcrecomp](https://github.com/sp00nznet/pcrecomp)**, as
  their own pull request; this repo keeps what is specific to this game.

## Where your code comes from

This repository is MIT. Code you write yourself is fine; code you adapted from
somewhere else has to be under a licence compatible with MIT, and the pull
request says where it came from. The DirectDraw compatibility projects you may
have open while working on the display layer differ: Wine's ddraw and DXGL are
LGPL, and code ported from them cannot come in. If you're unsure, ask in the
pull request first.

## AI-assisted contributions

Welcome, provided a person understood and checked the change before sending
it.
