# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*nib — flow-guided line drawing as an FFGL effect for Resolume; PUBLIC MIT, first release v0.1.0 (2026-08-22), never yet loaded into Resolume and no OpenFX port*

**nib** (`~/projects/resolume/nib`, `stoatworks-labs/nib`) — an FFGL effect that
turns a clip into **ink**: continuous, closed strokes on paper, not a glowing
outline. **PUBLIC MIT, released v0.1.0 on 2026-08-22** — macOS universal (dmg +
zip), Windows x64 (zip + NSIS installer), signed and notarised.

**The one idea.** A conventional edge effect asks how fast tone is changing at
each pixel, which is a magnitude with no memory of direction — it cannot tell a
stroke from a speck. nib builds an **edge tangent flow** field first, then steers
a difference of Gaussians *across* the flow and a line integral convolution
*along* it. Setting `Flow` to 0 skips the LIC and leaves plain isotropic XDoG,
and that A/B ships as a preset. The flow field is **measured**: `nibtest --flow`
renders rings whose tangent is known in closed form, adds noise, and reports the
error in degrees.

**Honest status:** never loaded into Resolume — everything is the offline
harness driving the real plugin class. No OpenFX port, no `--pipe`/`--script`,
so the fleet's video pipeline cannot film it. Measured only on macOS Apple
Silicon (0.66 ms/frame at 720p, 1.51 at 1080p, 6.60 at 4K).

## What its first release needed, none of which existed

It had been private with an **empty `.github/workflows` directory** — a tag would
have landed and nothing would have built, silently. Added on release day:

- `.github/workflows/release.yml`, taken from **outrun** (closest sibling: FFGL,
  no OFX) with the names and `NIB_BUILD_TOOLS` changed.
- `scripts/release-lib.sh`, vendored from the **fleet copy, not the backend
  master** — the master on `main` was stale ([release lib](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_release_lib.md)).
- `vcpkg.json`. Its absence failed the first CI run on
  `find_package(GLEW REQUIRED)`; it was the only FFGL repo without one.
- A `projects.json` entry, a `/video-plugins` suite row ("The pen", making the
  set seventeen), and `docs/thumb.png` rendered by `nibtest` and mapped in the
  website's `scripts/shots.json`.

⚠️ **Adding the projects.json entry grew the About block from two buttons to
three** (it gained a Project page URL) and **failed the build on the
static_assert** — `'3 == 4'` — rather than leaving the parameter undeclared. The
first time that guard has caught anything real. See [about window](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_about_window.md).

Related: [ofx ports](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/project_ofx_ports.md), [fleet mass release traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_fleet_mass_release_traps.md).
