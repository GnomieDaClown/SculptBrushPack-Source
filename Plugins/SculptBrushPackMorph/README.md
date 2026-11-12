# Sculpt Brush Pack: Morph Targets

An editor plugin for Unreal Engine 5.8 that adds six brushes for editing morph targets to the Skeletal Mesh Editor's
Sculpt Morph Target tool. Where an ordinary sculpt brush moves the surface, these work on what the morph target adds to
it: one strengthens it, one smooths it, two are for correctives sculpted in a pose, mending what skinning does to the
surface and raising wrinkles where it bunches up, and two copy it, from another morph target or mirrored from the other
side of the mesh.

It needs the Sculpt Brush Pack (`SculptBrushPack`) and the engine's experimental plugin "Skeletal Mesh Morph Target
Editing Tools". Built and tested against stock UE 5.8.4 (Win64).

## Installing

Copy the `SculptBrushPack` and `SculptBrushPackMorph` folders into your project's `Plugins` folder and enable both in the
Plugins window. A release archive's folders hold prebuilt binaries for Unreal Engine 5.8, so nothing has to be compiled;
without the `Binaries` folders, build the project. Enabling this plugin enables the engine's experimental morph target
tools with it.

## Using the brushes

1. Open a skeletal mesh. The Skeletal Mesh Editor's Editing Tools are on unless you switched them off.
2. In the Morph Targets tab, pick the morph target to edit with its radio button, and give it a weight above 0. The tool
   drops any stroke made while the edited morph target's weight is 0.
3. In the Morph palette, start Sculpt Morph Target.
4. The six brushes are the last in the brush palette, after the engine's and the Sculpt Brush Pack's.

They share one rule: the result of a stroke depends on where the brush went and how hard, not on how long it stayed.
A second pass over the same place adds nothing, and a lighter pass takes nothing back.

| Brush | What it does | How to use it |
|---|---|---|
| AmplifyMorph | Makes the morph stronger where you brush. | Brush the area. **Amount** sets how much stronger at full reach: 0.5 is half again as strong. Hold Ctrl to make it weaker instead; it stops at nothing and never turns the shape over. |
| SmoothMorph | Smooths what the morph adds, and leaves the model's own detail alone. | Brush a hard edge of the morph to soften it. **Smoothing** sets how broadly it smooths. |
| MushMorph | Mends what skinning does to the surface in a pose: a pinched elbow, a collapsed shoulder. The surface is evened out and the model's own detail is kept. | Pose the mesh, then brush the joint that looks wrong. **Smoothing** sets how broadly the surface is evened out: few rounds for a small pinch, more for a whole joint. On a mesh in its reference pose there is nothing to mend and the brush leaves it alone. |
| WrinkleMorph | Raises wrinkles where the posed surface bunches up, as on the inside of a bend. | Pose the mesh, then brush where the skin compresses. **Height** is the wrinkle's height; hold Ctrl to cut it in. See "Wrinkle Morph's pattern" below for maps and alphas. |

When the edited morph target's weight is 0 a stroke cannot act, and the tool's panel shows a warning that says so.

### Wrinkle Morph's pattern

How much a point rises is its **compression** times its **pattern**, times Height.

- **Compression.** With **Only Where Compressed** on, a point rises by how much shorter the surface around it is in the
  pose than at rest: nothing until it is **Starts At** shorter, fully from **Full At**. Turn it off to raise wrinkles
  everywhere the brush goes, which is also how to use the brush on a mesh in its reference pose. If a stroke finds
  nothing compressed under it, the tool's panel says so when the stroke ends.
- **Wrinkle Map.** A texture read at each point's place in the mesh's UVs: its red channel is the share of Height the
  point rises by, so black leaves the point and white raises it fully. **Mid Grey Is Flat** is for maps painted around
  mid grey, as displacement maps are: white raises, black lowers. **Map Tiling** repeats the map, and **UV Channel**
  picks the UVs it is read with. The texture is read when the brush is first used with it; pick the brush again after
  changing the texture's pixels. A normal map is not a height map and will not give the wrinkles it draws.
- **The brush's alpha.** The tool's own Alpha shapes the wrinkles as it shapes any stroke of these brushes: where the
  alpha is dark, less rises. As a stroke keeps the most each point has been given, a dragged stroke fills the alpha's
  gaps in; to stamp an alpha's pattern, click once or use the Dots stroke type.
- Without a map the rise is even, and compression and the alpha alone shape it.

The mesh needs enough points to show a wrinkle: a brush moves the points there are, and adds none.

### What Mush Morph cannot do

It needs a surface around each point: several triangles joined at their corners. Where the mesh is cut into separate
flat pieces of two triangles, as a cube with hard edges is, there is nothing to even out and the brush says it could
not measure the surface.

## Clone Morph and Mirror Morph

These two follow WrinkleMorph in the palette. To leave them out, enter `SculptBrushPackMorph.CloneAndMirror 0` in the
console and start Sculpt Morph Target again; `1` brings them back. The variable is not kept between sessions of the
editor.

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
  experimental itself for that reason. The build stops with a message on an engine other than 5.8. If one of the
  tool's members is renamed or removed, the build fails in that one header; if its type changes, a `static_assert`
  beside it fails. The header's comment lists what each member is taken to mean.
- No skinning is worked out in the plugin. A morph target's deltas are kept for the reference pose; what posing does to a
  delta at a vertex is taken from the engine's exported un-posing (`SkeletalMeshToolsHelper::GetUnposedMesh`).
- The brushes themselves know nothing of the tool. They work against the interface `IMorphBrushContext`
  (`Private/SculptBrushPackMorphBrushes.h`), which the module implements over the tool and the tests implement by hand.
- The brushes are handed to the Sculpt Brush Pack through its `AddBrushProvider`, for this one tool class only.

## Tests

`Private/Tests/SculptBrushPackMorphTests.cpp`, under the automation filter `SculptBrushPackMorph`:

- Six tests of the brushes' rules, compared exactly with values worked out by hand, including the warnings. Mush
  Morph's: a mesh that the pose only turns and moves is left as it is, and a bump the pose flattened gets its detail
  back. Wrinkle Morph's: the compression, a map read at texel centers, mid grey, tiling, an alpha, and Ctrl.
- A test that opens the Skeletal Mesh Editor on a copy of the engine's skeletal cube with two morph targets, starts
  Sculpt Morph Target and checks the palette and what the brushes read from the tool. The palette is checked three
  times: as the plugin is set (six morph brushes), with the console variable off after a tool was left on MirrorMorph
  (four, and the tool starts on the engine's Move), and with it on again (six). It also checks that the tool gives
  every point its place with the pose taken off and its place in the UVs.
- A test that makes a real stroke with each of the six brushes on that cube and reads the morph target back. Wrinkle Morph raises a corner with Only Where Compressed off, and with it on reports that
  nothing is compressed. Mush Morph reports that it cannot measure the cube's separate faces, and moves nothing.
- A test that leaves Sculpt Morph Target on a morph brush and then starts the plain Sculpt tool, which does not have it.

In the plugin's repository (https://github.com/GnomieDaClown/SculptBrushPack-Source), `pwsh -File Tools/BrushPack.ps1` builds both plugins in
their host project and runs the tests headless.

## What is not covered

- **Mush Morph and Wrinkle Morph have not been run on a posed mesh in the editor.** Their rules are tested on small
  meshes written out by hand, and their strokes on a cube in its reference pose, where Mush Morph cannot act and
  Wrinkle Morph finds no compression. Reading a real texture as a Wrinkle Map has not been run either: the tests give
  the map's values directly.
- The strokes in the tests are on a cube in its reference pose. Untested: a mesh with bones moved, a paused animation
  frame, part of the mesh isolated, the symmetry of a real character, pen pressure, a packaged build, engine versions
  other than 5.8.4.
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
prebuilt Win64 editor binary for Unreal Engine 5.8. Using it needs Unreal Engine, which comes under Epic Games' own
licence.
