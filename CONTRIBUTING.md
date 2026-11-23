# Contributing

Bug reports, test results, saves and code are all welcome. This page says what helps most.

Never attach or commit game files: disc images, `default.xex` or other files from the disc, the
generated code in `generated/default/`, or shaders and textures captured from the game.

## Reporting a bug

Check the [known issues](README.md#known-issues) first, then open an issue with the **Bug report**
template. Include:

- the version (or the commit, if you built it), your system (Linux distribution, Windows, or
  Proton), GPU and driver, and your RECOMP SETTINGS (render scale, anti-aliasing, V-Sync, Frame
  Pacing, Input Latency);
- where in the game it happens (chapter and place, or the menu) and what you did just before;
- a screenshot or a short video of a graphics glitch;
- for a **crash, freeze or a problem starting**: the log, `ng2.log` (where it is kept is in the
  [README](README.md#where-things-are-kept));
- for **stutter**: set `log_hitches = true` in the settings file, `ng2.toml` (same place), play the part that
  stutters, and attach the log;
- if it only happens in one place: a save from just before it.

To tell our bug from the game's own behaviour, compare with Xenia, or with footage of the Xbox 360 if
you can find it, and say what you saw there.

## Suggesting a feature

Open an issue and describe what you want and why. Changes that make the game look or play
differently from the Xbox 360 are added as options, off by default.

## Testing builds

Test builds are posted in issues. When you report back, include your GPU, driver, system and
settings, and what you tried: "fixed", "not fixed" and "different" are all useful. A screenshot of the
same spot before and after helps most.

## Code

Pull requests are welcome. A patch attached to an issue works too.

- Build from source as described in the [README](README.md#building-from-source).
  [AGENTS.md](AGENTS.md) has the project layout, the debugging tools, and the rules for test runs.
- Keep a pull request to one fix or feature, and say in it how you tested it (GPU, settings, where in
  the game).
- **Fixes** that make the port match the Xbox 360 can be on by default. Check them against Xenia.
- **Enhancements** that change the original look or behaviour go behind a setting, off by default.
- Changes to the SDK, XenosRecomp or plume are commits on the `ng2` branch of their forks
  (the submodules in `thirdparty/`), and the submodule's new commit in this repository. Don't edit `generated/`; it is regenerated from each
  person's disc.
- Commit messages start with the area when it helps (`Renderer:`, `Menus:`, `Audio:`, ...) and name
  the issue.

The project is licensed GPL-3.0-or-later, and contributions are accepted under the same license.
Contributors are credited in the release notes.
