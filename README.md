# Ninja Gaiden II Recompiled for ARM Linux

64-bit ARM (aarch64) Linux builds of [Ninja Gaiden II Recompiled](https://github.com/gamboozino/ng2recomp) by gamboozino. Same game and features as upstream, built for ARM handhelds and other aarch64 Linux devices. A new build is published here automatically after each upstream release.

No game data is included, you need your own copy of the World release with Title Update 3.

To install:

1. Download `NinjaGaiden2-linux-arm64.tar.gz` from [Releases](https://github.com/cerroverb/ng2recomp/releases/latest) and unpack it:

       tar -xzf NinjaGaiden2-linux-arm64.tar.gz

2. Put your extracted game (`default.xex` and the rest of the disc) in `NinjaGaiden2-arm64/game_files`, and Title Update 3's `default.xexp` next to `default.xex`. Upstream's [README](https://github.com/gamboozino/ng2recomp#building-from-source) shows how to get both from a disc image and the update package.
3. Optional: put the DLC packages (Mission Mode, costumes) in `NinjaGaiden2-arm64/game_files/dlc`.
4. Run `NinjaGaiden2-arm64/ng2`. For Steam's Game Mode, add `ng2` as a non-Steam game, no launch options needed.

Needs glibc 2.38 or newer and Vulkan 1.3 drivers. Settings (`ng2.toml`) and logs are kept next to `ng2`.

Bugs that also happen on PC go to [upstream](https://github.com/gamboozino/ng2recomp/issues), ARM-only ones here. That means before opening an issue here, first see whether it can be replicated on an upstream build.

The game's code is GPL-3.0-or-later like upstream.
