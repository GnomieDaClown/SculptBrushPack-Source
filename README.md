# Sculpt Brush Pack: source

The source of two editor plugins for Unreal Engine 5.8 that add sculpt brushes to the engine's own Vertex Sculpt tools.
They add no tool, mode or asset type of their own: the brushes appear in the brush palette of the tools you already use,
in Modeling Mode and in the Skeletal Mesh Editor.

| Plugin | What it adds |
|---|---|
| [`Plugins/SculptBrushPack`](Plugins/SculptBrushPack/README.md) | Eighteen brushes: Clay, Clay Strips, Crease, Blob, Dam Standard, Snake Hook, Move Topological, Layer, Fill, Scrape, Nudge, Thumb, TopoSlide, Relax, Glide, Tube, Noise and Sharpen. |
| [`Plugins/SculptBrushPackMorph`](Plugins/SculptBrushPackMorph/README.md) | Six brushes for morph targets, in the Skeletal Mesh Editor's Sculpt Morph Target tool: Amplify Morph, Smooth Morph, Mush Morph, Wrinkle Morph, Clone Morph and Mirror Morph. It needs the first plugin and the engine's experimental morph target editing tools. |

Each plugin's README says what every brush does, where it appears, how it gets into the engine's tool, how it is tested
and what is not covered.

**This is the `main` branch: the source for Unreal Engine 5.8.** The [`5.9`](https://github.com/GnomieDaClown/SculptBrushPack-Source/tree/5.9)
branch holds the source for Unreal Engine 5.9.

**This repository holds source only.** To use the plugins from here you compile them with your project. A packaged
version with prebuilt binaries, which needs no compiling, is available from the author on request.

The brushes are part of a larger modeling suite that is still in the works, so some functionality may not be complete.
Bug reports and feature requests are welcome in this repository's issues.

## Branches

`main` is the released source for Unreal Engine 5.8. `dev` is where the next version is being worked on, and may not be
finished or stable. `5.9` is the released source ported to Unreal Engine 5.9.

## Requirements

- **Unreal Engine 5.8.** Built and tested on stock 5.8.4, Win64. The build stops with a message on any other engine
  version: the plugins name protected members of the engine's sculpt tools, which can change from one version to the
  next.
- Windows, with the C++ toolchain Unreal Engine 5.8 asks for, and a project with C++ targets.

## Building into a project

1. Copy the folders `Plugins/SculptBrushPack` and `Plugins/SculptBrushPackMorph` into your project's `Plugins` folder.
   Leave the second out if you do not want the morph brushes; it cannot be used without the first.
2. Enable them in the editor's Plugins window, or in your `.uproject`:

   ```json
   "Plugins": [
       { "Name": "SculptBrushPack", "Enabled": true },
       { "Name": "SculptBrushPackMorph", "Enabled": true }
   ]
   ```

   Modeling Mode's Vertex Sculpt also needs the engine plugin `ModelingToolsEditorMode` enabled, if it is not already.
3. Close the Unreal editor and build your project's editor target.
4. The brushes are at the end of the brush palette, after the engine's own:
   - Modeling Mode: Deform palette, Vertex Sculpt.
   - Skeletal Mesh Editor, with its Editing Tools on: Deform palette, Sculpt; Morph palette, Sculpt Morph Target.

Each plugin has automation tests, under the filters `SculptBrushPack` and `SculptBrushPackMorph` in the editor's Session
Frontend: 24 in all.

## Building and testing in this repository

```
pwsh -File Tools/BrushPack.ps1
```

builds both plugins in the host project `Hosts/BrushPack58` and runs their automation tests in a headless editor. The
engine folder is taken from `-EngineRoot`, else from the environment variable `UE_ROOT`, else from the Epic Games
launcher's list of installed engines. Close the Unreal editor first: the engine's build tool refuses to build while an
editor with Live Coding is running. [`CLAUDE.md`](CLAUDE.md) holds the instructions for Claude and other coding agents.

## Licence

Copyright (C) 2026 Joseph Wiginton.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [`LICENSE`](LICENSE) for the full text.

Fourteen of the pack's brushes, Clay to Relax, follow displacement rules and constants from
[Blender](https://projects.blender.org/blender/blender)'s sculpt source (Copyright Blender Authors, GNU General Public
License version 2 or later), cited by file and line in the pack's source. Glide, Tube, Noise, Sharpen and the morph
brushes are this project's own designs.

The repository contains no Unreal Engine code and no code copied from Blender. Using the plugins needs Unreal Engine,
which comes under Epic Games' own licence.
