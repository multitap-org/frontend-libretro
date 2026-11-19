# frontend-libretro

What every multitap libretro frontend shares on top of host-libretro and
frontends-common: the core session (run a core in a window: video texture,
audio, timing, input, savestates), core options storage, keyboard and fixed
controls, and the catalog of installed cores. No UI of its own.

A library: it doesn't build on its own. A frontend adds it as a submodule,
after host-libretro and frontends-common.

MIT License (see LICENSE).
