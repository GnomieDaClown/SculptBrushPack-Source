# CLAUDE.md

Instructions for Claude agents, and any other coding agent, working with the Sculpt Brush Pack: setting the plugins up in
someone's Unreal project, or building, testing and changing them in this repository.

Repository: https://github.com/GnomieDaClown/SculptBrushPack-Source (public; the source, and where development happens)

## What this is

- Two Unreal Engine **editor** plugins. A release archive holds their source and prebuilt Win64 editor binaries for
  Unreal Engine 5.8, so they run without being compiled. The repository holds source only:
  - `Plugins/SculptBrushPack` adds eighteen brushes to the engine's Vertex Sculpt tools (Modeling Mode's Vertex Sculpt, the
    Skeletal Mesh Editor's Sculpt and Sculpt Morph Target).
  - `Plugins/SculptBrushPackMorph` adds morph target brushes to Sculpt Morph Target: Amplify Morph and Smooth Morph. Two
    more, Clone Morph and Mirror Morph, are in the code but out of the palette unless the console variable
    `SculptBrushPackMorph.CloneAndMirror` is on. This plugin needs the first one and the engine's experimental plugin
    `SkeletalMeshMorphTargetEditingTools`, which its descriptor enables.
- Written for **Unreal Engine 5.8**; built and tested on stock 5.8.4, Win64, MSVC. Nothing else has been tried.
- Each plugin's `README.md` is the reference for what the brushes do, how they get into the engine's tools, how they are
  tested and what is not covered. Read it before changing that plugin.

If you were given only a release archive (`SculptBrushPack-<version>.zip`), you have the two plugin folders with their
binaries, `LICENSE`, `README.md` and this file. The host project and the scripts named below are in the repository above.

## Before you do anything: find out three things

1. **Where Unreal Engine 5.8 is**: the folder that holds `Engine/`. Look, in this order, at what the person told you, the
   environment variable `UE_ROOT`, and the Epic Games launcher's list
   `C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat` (the entry whose `AppName` is `UE_5.8`; its
   `InstallLocation` is the folder). If none gives it, ask. Do not guess a path, and do not install or update an engine.
   Check the version in `<engine>/Engine/Build/Build.version`: it must be 5.8.
2. **Which project gets the plugins**: the path of its `.uproject`. To only build and test the plugins, use this
   repository's host project and skip to "Building and testing in this repository".
3. **Whether the Unreal editor is open.** The engine's build tool refuses to build while an editor with Live Coding is
   running, with "Unable to build while Live Coding is active". Ask the person to close the editor. Never end the
   editor's process yourself: it may hold unsaved work.

## Setting the plugins up in a project

1. Copy `Plugins/SculptBrushPack` and `Plugins/SculptBrushPackMorph` into `<project>/Plugins/`, keeping the folder names.
   If the person does not want the morph brushes, copy only `SculptBrushPack`. Never copy `SculptBrushPackMorph` alone.
   From a release archive, copy the folders whole, `Binaries` included: with them nothing has to be compiled, and the
   steps below that build can be skipped. Checked in a Blueprint-only project on stock 5.8.4. Never copy `Intermediate`.
2. In the project's `.uproject`, add to the `Plugins` array (create the array if there is none), keeping what is there:

   ```json
   { "Name": "SculptBrushPack", "Enabled": true },
   { "Name": "SculptBrushPackMorph", "Enabled": true }
   ```

   For Vertex Sculpt in Modeling Mode the engine plugin `ModelingToolsEditorMode` must be enabled too; add
   `{ "Name": "ModelingToolsEditorMode", "Enabled": true, "TargetAllowList": [ "Editor" ] }` if the project does not
   have it. Ask before enabling anything else.
3. With the archive's binaries in place, open the project; that is all. To compile instead (no binaries, or changed
   source), build the project's editor target and run the tests. From a clone of this repository:

   ```
   pwsh -File Tools/BrushPack.ps1 -Project "<full path to the .uproject>" -EngineRoot "<engine folder>"
   ```

   Without the script, these are the two commands it runs (PowerShell):

   ```
   & "<engine>/Engine/Build/BatchFiles/Build.bat" <ProjectName>Editor Win64 Development "-Project=<full path to the .uproject>" -WaitMutex -NoHotReload

   & "<engine>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<full path to the .uproject>" "-ExecCmds=Automation RunTests SculptBrushPack; Quit" "-TestExit=Automation Test Queue Empty" "-ReportExportPath=<an empty folder>" -unattended -nopause -nosplash -nullrhi -nosound -stdout -FullStdOutLogOutput
   ```

   The test results are in `<that folder>/index.json`: every entry of `tests` should have `"state": "Success"`.
   This needs a project with C++ targets (`<ProjectName>Editor`). It has not been tried with a Blueprint-only project;
   if that is what the person has, say so and ask how they want to go on.
4. Tell the person where the brushes are: at the end of the brush palette, after the engine's own, in Modeling Mode
   (Deform, Vertex Sculpt) and in the Skeletal Mesh Editor with its Editing Tools on (Deform, Sculpt; Morph, Sculpt
   Morph Target). You have not seen them there yourself unless you drove the editor; say which it is.

## Building and testing in this repository

```
pwsh -File Tools/BrushPack.ps1
```

It builds `BrushPackHost58Editor Win64 Development` for the host project `Hosts/BrushPack58`, which finds the plugins in
`Plugins/`, then starts a headless editor and runs the automation tests under the filter `SculptBrushPack`. Useful
arguments: `-EngineRoot <folder>`, `-Step Build` or `-Step Test`, `-Filter SculptBrushPack.Brushes`.

**What success looks like.** The last lines read like this, and the exit code is 0:

```
Build (BrushPackHost58Editor Win64 Development): exit 0, 0 errors, 0 warnings, ... s
Tests (SculptBrushPack): 22 passed, 0 failed, of 22; editor exit 0; ... s
```

Twenty-two tests: fifteen under `SculptBrushPack.` and seven under `SculptBrushPackMorph.`. Logs are in `Build/Logs`.

**Things that are not failures.**

- The test log holds one engine warning, "Element 'null - ID: 24' is still externally referenced when being destroyed",
  as a Skeletal Mesh Editor the tests opened is closed. No test fails on it.
- A test run makes the editor add a section with a generated security token to
  `Hosts/BrushPack58/Config/DefaultEngine.ini`. Put the file back with
  `git restore Hosts/BrushPack58/Config/DefaultEngine.ini` and never commit that section.
- Opening the host project by hand makes the editor write `Config/DefaultEditor.ini` and `Config/DefaultInput.ini` into
  it. They are ignored by git; do not commit them.

**When it does fail.**

| What you see | What it means | What to do |
|---|---|---|
| "Unable to build while Live Coding is active" | An Unreal editor is open. | Ask for it to be closed. Do not end its process. |
| `#error` naming Unreal Engine 5.8 | The engine is another version. | Stop and say so. Do not remove the check (see the rules). |
| "No test ran" | The plugins did not load, or the filter matched nothing. | Read the test log for `LogSculptBrushPack` and `LogPluginManager` lines. |
| `SculptBrushPack.Hook.VertexSculptInModelingMode` fails in another project | `ModelingToolsEditorMode` is not enabled there. | Enable it, with the person's agreement. |

## Rules for changing the code

- **Never edit engine or Epic plugin source** to make these plugins work, in any engine folder.
- **The engine version check stays.** Both plugins reach protected members of the engine's sculpt tools through a class
  derived from the tool and never created: `FToolAccess` in `SculptBrushPackModule.cpp` and `FMorphToolAccess` in
  `SculptBrushPackMorphToolAccess.h`. The comments there list each member and what it is taken to mean. Supporting
  another engine version means reading those members again in that engine's source, not deleting the `#error`.
- **The tests compare exactly, with no tolerance.** Do not add a tolerance or loosen an assertion to make a test pass;
  find out why the value differs. `FPSemantics = FPSemanticsMode.Precise` in both `Build.cs` files is what the exact
  comparisons rely on and stays.
- **A change is not done until `Tools/BrushPack.ps1` has run and you have read its result.** A new brush or rule comes
  with a test of its rule.
- **Ask the owner before** adding a module or plugin dependency, adding a brush of a new design, or changing what a
  brush does by default.
- **Licence.** Everything here is under the GNU GPL, version 3 or later. Every source file begins with the two lines

  ```
  // Copyright (C) 2026 Joseph Wiginton.
  // SPDX-License-Identifier: GPL-3.0-or-later
  ```

  and a new file gets them too. Brushes that follow Blender's sculpt source cite it by file, line and tag; keep those
  citations, and add one for any rule you take from there. Do not copy code from the engine or from another plugin into
  this repository.
- **No binaries in git.** Never commit `Binaries` or `Intermediate`. A release archive carries prebuilt binaries, which
  `Tools/Package.ps1 -BinariesFrom` takes from a project where the same commit was built and its tests passed.
- **Nothing personal in the repository.** No local paths, user names or addresses of the person you work for in files,
  commit messages or release notes.
- Identifiers of the pack's brushes are 2001 to 2018 and of the morph brushes 2101 to 2104. A brush that another module
  hands the pack through `SculptBrushPack::AddBrushProvider` needs an identifier of 1000 or more that is not in use.

## Git, branches and releases

- Development happens in this repository, on `dev`. `main` changes only by merging `dev` through a pull request, at a
  release. Do not push to `main`. The branch `5.9` holds the port to Unreal Engine 5.9; what reaches `main` is brought
  to `5.9` afterwards, with the engine line numbers in the comments read again for 5.9.
- **Pushing, opening or merging a pull request, making a release and changing the repository's visibility are the
  owner's decisions.** Ask each time unless you were told to go ahead.
- This repository is public: every commit pushed here can be read by anyone. The release archives with prebuilt
  binaries are made from it and published by the owner in a separate, private repository.
- A release, once the owner asks for one:
  1. Set `VersionName` and `Version` in `Plugins/SculptBrushPack/SculptBrushPack.uplugin` (and the morph plugin's, if it
     changed).
  2. Run `pwsh -File Tools/BrushPack.ps1`; every test passes.
  3. Merge `dev` into `main` by pull request.
  4. On `main`: build and test the plugins in a project that has them in its `Plugins` folder
     (`pwsh -File Tools/BrushPack.ps1 -Project <that project>`), then
     `pwsh -File Tools/Package.ps1 -BinariesFrom <that project's folder>`, which writes
     `Build/SculptBrushPack-<version>.zip` from committed files and those binaries. Run the tests once more in a
     project without source targets that holds only the archive's plugin folders
     (`pwsh -File Tools/BrushPack.ps1 -Step Test -Project <it>`): that is the proof it runs without compiling.
  5. The owner publishes the archive, with notes that say what changed, what was tested and what was not. Archives are
     released in the private repository, not here.

## Reporting

Say what you built and ran, with the script's summary lines. Say what you did not verify. The automated tests run in a
headless editor: that a brush feels right under a pen or a mouse is known only if a person tried it, so do not report it
as tested.
