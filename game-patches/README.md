# Game patches

Game-side fixes for problems in RPCS3 that the emulator does not fix yet. They use RPCS3's normal patch format.

## Gran Turismo 5

| Patch | Version | Status |
|---|---|---|
| Disable Shadow Optimize (bumper cam darkening fix) | BCES00569 02.11 (EU) | tested in-game |
| Disable Shadow Optimize (bumper cam darkening fix) | BCUS98114 02.17 (US) | ported, not tested yet |

**Bug:** in bumper cam the track turns dark as soon as no other car is in view, for example when leading at Tsukuba Circuit.
Upstream: [RPCS3#11912](https://github.com/RPCS3/rpcs3/issues/11912); [RPCS3#19653](https://github.com/RPCS3/rpcs3/pull/19653) fixed a related texture cache bug but not this case.

**Cause:** GT5 fits the shadow map depth range to the cars in view ("shadow optimize").
With no car in view the fit is skipped and a stale range stays active, so the track is drawn as shadowed.
The patch makes the game always use the course's static shadow range. Shadows can be slightly less sharp.

## Install

1. Copy the file for your game ID into RPCS3's `patches` folder:
   - Linux: `~/.config/rpcs3/patches/`
   - Windows: `<RPCS3 folder>\patches\`
2. In RPCS3 open **Manage > Game Patches**, search for the game and tick the patch.

Do not rename the files: RPCS3 loads `<GAME_ID>_patch.yml` automatically for that game.
The patch only applies to the exact executable listed above (checked by its `PPU-` hash).

## Issues

Report problems in this fork's [issue tracker](https://github.com/Cue-System90/rpcs3/issues).
