# Sculpt Brush Pack: Morph Targets

An editor plugin for Unreal Engine 5.9 that adds two brushes for editing morph targets to the Skeletal Mesh Editor's
Sculpt Morph Target tool. Where an ordinary sculpt brush moves the surface, these work on what the morph target adds to
it: one strengthens it, the other smooths it. Two more brushes, which copy it from another morph target and mirror it
across the mesh, are in the plugin but out of the palette for now; see "Clone Morph and Mirror Morph" below.

It needs the Sculpt Brush Pack (`SculptBrushPack`) and the engine's experimental plugin "Skeletal Mesh Morph Target
Editing Tools". Built and tested against stock UE 5.9.0 (Win64).

## Installing

Copy the `SculptBrushPack` and `SculptBrushPackMorph` folders into your project's `Plugins` folder and enable both in the
Plugins window. A release archive's folders hold prebuilt binaries for Unreal Engine 5.9, so nothing has to be compiled;
without the `Binaries` folders, build the project. Enabling this plugin enables the engine's experimental morph target
tools with it.

## Using the brushes

1. Open a skeletal mesh. The Skeletal Mesh Editor's Editing Tools are on unless you switched them off.
2. In the Morph Targets tab, pick the morph target to edit with its radio button, and give it a weight above 0. The tool
   drops any stroke made while the edited morph target's weight is 0.
3. In the Morph palette, start Sculpt Morph Target.
4. The two brushes are the last in the brush palette, after the engine's and the Sculpt Brush Pack's.

They share one rule: the result of a stroke depends on where the brush went and how hard, not on how long it stayed.
A second pass over the same place adds nothing, and a lighter pass takes nothing back.

| Brush | What it does | How to use it |
|---|---|---|
| AmplifyMorph | Makes the morph stronger where you brush. | Brush the area. **Amount** sets how much stronger at full reach: 0.5 is half again as strong. Hold Ctrl to make it weaker instead; it stops at nothing and never turns the shape over. |
| SmoothMorph | Smooths what the morph adds, and leaves the model's own detail alone. | Brush a hard edge of the morph to soften it. **Smoothing** sets how broadly it smooths. |

When the edited morph target's weight is 0 a stroke cannot act, and the tool's panel shows a warning that says so.

## Clone Morph and Mirror Morph

These two brushes are built and tested, but the plugin does not add them to the palette for now. The console variable
`SculptBrushPackMorph.CloneAndMirror` brings them in: enter `SculptBrushPackMorph.CloneAndMirror 1` in the console and
start Sculpt Morph Target again; they follow SmoothMorph. Set it to `0` and start the tool again to take them out. The
variable is not kept between sessions of the editor.

| Brush | What it does | How to use it |
|---|---|---|
| CloneMorph | Paints another morph target's shape into the one you are editing. | Choose the morph target to copy from in **Source**, then brush where its shape should appear; when the mesh has only one other morph target, it is the source without being chosen. The Source is chosen anew each time the tool starts. **Amount** scales it. With **Add To Shape** the source is added to what is there; without it, it replaces what is there. Hold Ctrl to take the source's shape out instead. |
| MirrorMorph | Gives the points you brush the shape of the points across the mesh's symmetry plane, mirrored. | With **Copy From** at Other Side, brush the side that should receive the shape. To be safe against brushing away the side you sculpted, set Copy From to the side you sculpted, +X Side or -X Side: then only the opposite side changes. |

The tool's panel shows a warning when one of them cannot act:

- CloneMorph has no Source, or its Source is not a morph target of this mesh, or the mesh has no other morph target;
- MirrorMorph finds no partner across the symmetry plane for the points under the brush;
- MirrorMorph is used while the tool's own Symmetry is on. Symmetry copies the +X side onto the -X side under every
  brush by itself; turn it off to mirror with the brush.

CloneMorph works while Symmetry is on, and says what Symmetry then does: only the source's +X half is cloned, onto both
sides.

### What MirrorMorph needs from the mesh

The symmetry plane is X = 0 in the mesh's reference pose, and a point's partner is the one the editor itself pairs it
with, as for the editor's own Mirror and Flip commands on morph targets. Points the editor cannot pair are left alone. A
point on the plane keeps the part of its shape that lies in the plane.

## How it reads the tool

A morph brush has to know where each point stands without the edited morph target, what the other morph targets would do
there, and how the mesh is posed. That is the tool's own state, and the tool offers no public way to read it. The plugin
reads it through a class that derives from the tool and is never created (`FMorphToolAccess` in
`Private/SculptBrushPackMorphToolAccess.h`), which may form pointers to the tool's protected members.

- The plugin is tied to the engine version it is built for, and to a plugin the engine marks experimental; it is marked
  experimental itself for that reason. The build stops with a message on an engine other than 5.9. If one of the
  tool's members is renamed or removed, the build fails in that one header; if its type changes, a `static_assert`
  beside it fails. The header's comment lists what each member is taken to mean.
- No skinning is worked out in the plugin. A morph target's deltas are kept for the reference pose; what posing does to a
  delta at a vertex is taken from the engine's exported un-posing (`SkeletalMeshToolsHelper::GetUnposedMesh`).
- The brushes themselves know nothing of the tool. They work against the interface `IMorphBrushContext`
  (`Private/SculptBrushPackMorphBrushes.h`), which the module implements over the tool and the tests implement by hand.
- The brushes are handed to the Sculpt Brush Pack through its `AddBrushProvider`, for this one tool class only.

## Tests

`Private/Tests/SculptBrushPackMorphTests.cpp`, under the automation filter `SculptBrushPackMorph`:

- Four tests of the brushes' rules, compared exactly with values worked out by hand, including the warnings.
- A test that opens the Skeletal Mesh Editor on a copy of the engine's skeletal cube with two morph targets, starts
  Sculpt Morph Target and checks the palette and what the brushes read from the tool. The palette is checked three
  times: as the plugin is set (two morph brushes), with the console variable on (four), and with it off again after a
  tool was left on MirrorMorph (two, and the tool starts on the engine's Move).
- A test that makes a real stroke with each of the four brushes on that cube, with the console variable on, and reads
  the morph target back.
- A test that leaves Sculpt Morph Target on a morph brush and then starts the plain Sculpt tool, which does not have it.

In the plugin's repository (https://github.com/GnomieDaClown/SculptBrushPack), `pwsh -File Tools/BrushPack.ps1` builds both plugins in
their host project and runs the tests headless.

## What is not covered

- The strokes in the tests are on a cube in its reference pose. Untested: a mesh with bones moved, a paused animation
  frame, part of the mesh isolated, the symmetry of a real character, pen pressure, a packaged build, engine versions
  other than 5.9.0.
- The warnings are tested as text handed to the tool; that the editor shows them in its panel was read in the engine's
  source, not checked on screen.
- The icons are the nearest ones in the project's icon library, not drawn for these brushes.

## Licence

Copyright (C) 2026 Joseph Wiginton.

This plugin is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It
is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details: the file
`LICENSE` in this folder, or <https://www.gnu.org/licenses/>.

It needs the Sculpt Brush Pack, which is under the same licence and whose brushes in part follow rules from Blender's
sculpt source. The morph brushes themselves are this project's own designs.

The plugin contains no Unreal Engine code and no code copied from Blender. Release archives hold its source and a
prebuilt Win64 editor binary for Unreal Engine 5.9. Using it needs Unreal Engine, which comes under Epic Games' own
licence.
