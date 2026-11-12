# Sculpt Brush Pack

An editor plugin for Unreal Engine 5.8 that adds nineteen brushes to the engine's own Vertex Sculpt tools. It has one
editor module, depends on engine modules only, and adds no tool, mode or asset type of its own: the brushes appear in the
brush palette of the tools you already use.

Built and tested against stock UE 5.8.4 (Win64). It is not built for other engine versions; see "How it gets into the
tool" for why the version matters.

## Installing

Copy the `SculptBrushPack` folder into your project's `Plugins` folder and enable the plugin in the Plugins window. A
release archive's folder holds a prebuilt binary for Unreal Engine 5.8, so nothing has to be compiled; without the
`Binaries` folder, build the project. The plugin needs the engine plugins `MeshModelingToolset` and
`GeometryProcessing`, which Modeling Mode already enables.

## Where the brushes appear

At the end of the brush palette, after the engine's own brushes, in:

- **Modeling Mode**: Deform palette, Vertex Sculpt.
- **The Skeletal Mesh Editor**, with its Editing Tools on: Deform palette, Sculpt; and Morph palette, Sculpt Morph Target
  (the engine's experimental morph target tools plugin).

Each brush shows its own settings while it is the active brush. Size, stroke type, alpha and symmetry are the tool's and
work as for the engine's brushes. Ctrl inverts a brush where that means something.

Setting the console variable `SculptBrushPack.Enabled` to `0` makes sculpt tools start without the pack's brushes.

## The brushes

| Brush | What it does | Its own settings |
|---|---|---|
| Clay | Moves points toward a plane a little above the surface under the cursor: builds up and levels at once. | Plane Offset |
| ClayStrips | As Clay, with a square tip that lays down flat strips along the stroke. | Plane Offset, Tip Roundness, Tip Scale, Plane Trim |
| Crease | Cuts in along the stroke and pinches the surface toward it. Ctrl raises a pinched ridge. | Pinch |
| Blob | Pushes out and spreads the surface: rounded bulges. | Pinch |
| DamStandard | A sharp cut with pinched walls. It reads the surface from before the stroke, so the cut does not chase itself. | Pinch, Sharp Profile |
| SnakeHook | Pulls the surface along with the cursor and takes up what it passes over. | Pinch |
| MoveTopo | The engine's Move, limited to the piece of surface joined to the point under the cursor: the other lip of a closed mouth stays. | none |
| Layer | Raises the surface by a set height, no more, however often the stroke passes. | Height |
| Fill | Moves only what lies below the brushed area's average plane up toward it. | Plane Offset, Height, Depth |
| Scrape | Moves only what lies above that plane down toward it. | Plane Offset, Height, Depth |
| Nudge | Slides points along the surface with the cursor. | none |
| Thumb | Smears the surface along itself from where the stroke began. | none |
| TopoSlide | Slides points along the mesh's own edges in the direction of the stroke, which keeps the shape closely. | none |
| Relax | Evens out the spacing of points without changing the shape. | none |
| Glide | Moves points with the cursor and lays each back onto the surface as it was before the stroke. | none |
| Tube | Draws a bead with a round cross-section along the stroke. Ctrl cuts a round channel. | Height |
| Noise | Shows a bump pattern that is fixed in the mesh's space: any brush size shows the same bumps. | Height, Scale |
| Sharpen | Grows each point's own relief, bumps up and hollows down, by a set share. Ctrl takes relief away. | Amount, Detail Size |
| LegacyWrinkle | Draws wrinkle lines along the stroke: several fine cuts side by side, each with a raised lip. Ctrl raises ridges instead. | Depth, Lines, Spread, Line Width, Lip, Pinch |

Tube, Noise and Sharpen share one rule: the result of a stroke depends on where the brush went and how hard, not on how
long it stayed. A second pass over the same place adds nothing, and a lighter pass takes nothing back.

### LegacyWrinkle

The wrinkle brush of the familiar kind: you draw each wrinkle where you want it, and it deepens for as long as the brush
stays or passes again. It works on any mesh, posed or not, in all three tools.

- **Lines** is how many cuts run side by side (1 to 9), and **Spread** how far from the middle of the brush the
  outermost run. One line runs through the middle.
- **Line Width** is a cut's width as a share of the brush radius, **Depth** how strongly it is cut, **Lip** how high
  the surface rises beside each cut, and **Pinch** how strongly the points beside a line are drawn toward it.
- The lines follow the way the stroke moves, so the brush draws when it is dragged: a click without a drag leaves no
  mark.
- The mesh needs points enough to hold lines this fine: a brush moves the points there are, and adds none.

To raise wrinkles where a pose bunches the surface up, without drawing each one, see Nu Wrinkle in the Sculpt Brush
Pack: Morph Targets plugin.

## How it gets into the tool

The engine has no public call that adds a brush to its sculpt tools. `UMeshSculptToolBase::RegisterBrushType` is
protected, meant for subclasses, and a subclass would need its own button in every editor that hosts the tool.

The plugin instead listens to the tool managers (`UInteractiveToolManager::OnToolPostBuild` and `OnToolPostSetup`). When a
Vertex Sculpt tool has been built and before its `Setup` runs, it registers its brushes on the tool itself, naming the
tool's protected members through a class that derives from the tool and is never created (`FToolAccess` in
`SculptBrushPackModule.cpp`). After `Setup` it puts the engine's brushes first again in the palette.

Consequences a maintainer should know:

- The plugin is tied to the engine version it is built for. The build stops with a message on an engine other than 5.8.
  If one of the tool's protected members is renamed or removed, the build fails in that one struct; if its type
  changes, a `static_assert` beside it fails. What a member means cannot be checked by the compiler: the comment at
  `FToolAccess` lists what is assumed and has to be read again for another engine version.
- It reaches three tool classes: `UMeshVertexSculptTool` by its class, and by class path the engine's
  `MorphTargetVertexSculptTool` and `ModelingSuiteVertexSculptTool`. Other subclasses are left alone.
- Tool managers are found as the engine creates them (`FUObjectArray::FUObjectCreateListener`), and for the level
  editor's modes when a mode is entered. Code that makes a tool manager and starts a tool in the same frame calls
  `SculptBrushPack::WatchToolManagers()` in between.
- If the engine made `RegisterBrushType` public, or gave the sculpt tools an extension point for brushes, the access
  struct and the tool manager listening could both go.

Two things the plugin does to the tool's settings, both logged under `LogSculptBrushPack`:

- **The brush remembered from last time.** The engine remembers the last brush used under one key that Sculpt and Sculpt
  Morph Target share, and a tool started on a brush it does not have fails an engine `ensure`. Before a tool's `Setup`,
  when the remembered brush is one this plugin or a provider added and this tool is not getting it, the engine's Move
  is remembered in its place.
- **Freeze Target.** Ten brushes work from the surface as it was before the stroke: DamStandard, MoveTopo, Layer, Thumb,
  TopoSlide, Relax, Tube, Noise, Sharpen and LegacyWrinkle. While the tool's Freeze Target is set, that surface is not refreshed, and such a
  brush would undo earlier strokes under it. The plugin clears Freeze Target when one of these brushes becomes active.

## Adding brushes from another module

`SculptBrushPack::AddBrushProvider` (in `SculptBrushPack.h`) lets another module hand the pack brushes for the tools
it chooses. A provider is asked on the game thread for each tool the pack reaches, before the tool's `Setup`; its brushes
are registered after the pack's own and follow them in the palette. A brush whose identifier is below 1000, already in use on the tool, or without its two
callbacks is left out with a warning in the log. Remove the provider before the module that owns it unloads. The plugin `SculptBrushPackMorph` uses
this.

The public headers are `SculptBrushPack.h` (the calls above) and `SculptBrushPackSettings.h` (the settings base class a
provider's brushes may derive from). The brushes themselves are private to the module.

## Where the rules come from

- Clay to Relax (fourteen brushes) take their displacement rules and constants from Blender's sculpt source, tags v5.2.3
  and v4.2.0, cited by file and line in `Private/SculptBrushPackBrushes.h` and `.cpp`. They run on Unreal's own stroke, strength
  and falloff, so a given strength setting is not the same strength as in Blender. Blender is licensed under the GNU
  General Public License, and for that reason this plugin is too (see "Licence").
- Glide, Tube, Noise and Sharpen are this project's own designs.
- LegacyWrinkle is this project's own too. It does what wrinkle brushes in sculpting programs commonly do, lines cut
  along the stroke with a lip and a pinch, and takes no code, rule or number from any of them.

## Tests

`Private/Tests/SculptBrushPackTests.cpp`, under the automation filter `SculptBrushPack`:

- Fourteen tests of the brushes' rules. Each hands a brush a few stamps on a few points and compares with values worked
  out by hand, exactly, with no tolerance.
- Two tests that start the tool: Vertex Sculpt in Modeling Mode, and Sculpt in the Skeletal Mesh Editor. Each checks that
  the pack's brushes follow the engine's in the palette and can each be made the active brush.

In the plugin's repository (https://github.com/GnomieDaClown/SculptBrushPack-Source), `pwsh -File Tools/BrushPack.ps1` builds the plugin in its
host project and runs the tests headless.

## What is not covered

- The automated tests that start a tool check the palette; they make no stroke. (The morph plugin's tests drag one
  LegacyWrinkle stroke through Sculpt Morph Target and check that points moved.) The brushes have been tested by hand
  in the editor by the author, except LegacyWrinkle, which is new and has not been tried by hand yet.
- Untested: symmetry and alphas with these brushes, pen pressure, Freeze Target being cleared, the module being
  unloaded while a tool runs, a packaged build, engine versions other than 5.8.4.
- The tests compare exactly, which relies on the module being built with precise floating point (set in its
  `Build.cs`) and on one compiler; they were run with MSVC on Win64 only.
- The cost of Glide's and Sharpen's first stamp on dense meshes is not measured.
- The icons are the nearest ones in the project's icon library, not drawn for these brushes.

## Licence

Copyright (C) 2026 Joseph Wiginton.

This plugin is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It
is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details: the file
`LICENSE` in this folder, or <https://www.gnu.org/licenses/>.

Fourteen of its brushes follow rules and constants from Blender's sculpt source (<https://projects.blender.org/blender/blender>,
Copyright Blender Authors, GNU General Public License version 2 or later); see "Where the rules come from".

The plugin contains no Unreal Engine code and no code copied from Blender. Release archives hold its source and a
prebuilt Win64 editor binary for Unreal Engine 5.8. Using it needs Unreal Engine, which comes under Epic Games' own
licence.
