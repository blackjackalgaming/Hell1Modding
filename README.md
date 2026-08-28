# Hell1Modding

A native Lua mod loader for **Hades** (Supergiant Games), built as a fork of
[ReturnOfModdingBase](https://github.com/xiaoxiao921/ReturnOfModdingBase).

It is to Hades what [Hell2Modding](https://github.com/SGG-Modding/Hell2Modding)
is to Hades II — mods live in a profile folder, load into the game's own Lua
state, and nothing on disk is modified.

> **Status: working, not yet published.** Everything below is implemented and
> tested against the shipping game. Thunderstore does not have a Hades
> community yet, so installation is manual for now and there is no r2modman
> entry. The loader already implements r2modman's specification (see
> [Mod Manager Integration](#mod-manager-integration)), so it will work the
> moment one exists.

---

## What it replaces, and what it does not

**Replaces ModImporter.** ModImporter rewrites
`Content/Scripts/RoomManager.lua` on disk to append `Import` lines.
Hell1Modding does the same job from memory: it hooks the engine's script
loader, so your game files are never touched and a Steam update cannot undo
your setup.

**Does not replace ModUtil.** ModUtil is a Lua library and keeps working. In
fact **ModUtil 4.0.1** — the same release Hades II uses — loads and runs under
Hell1Modding, along with ENVY, Chalk, ReLoad, SJSON and DemonDaemon.

## Installing the mod loader

1. Grab `d3d11.dll` from a release (or build it, see [Building](#building)).
2. Drop it next to `Hades.exe`, which is in the **`x64`** folder of your Hades
   install — not the folder above it.
3. Launch the game. A `ReturnOfModding` folder appears next to `Hades.exe`
   containing `LogOutput.log`, `config/`, and `plugins/`.

Only the **`x64` (DirectX 11)** build is supported. `x64Vk` and `x86` each need
their own proxy and are untested.

> **If you launch the Vulkan build, the loader silently does not run.**
> Hades 1 picks its renderer by which executable you start, not by a launch
> option — `x64\Hades.exe` is DirectX 11, `x64Vk\Hades.exe` is Vulkan. The
> Vulkan engine never imports `d3d11.dll`, so the proxy is never loaded: no
> mods, no log, no error. (There is no DirectX 12 build; `-dx12` as a Steam
> launch option does nothing here.)

To uninstall, delete `d3d11.dll`. Nothing else is modified.

---

## User Interface

Comes with an ImGui user interface.

The default key to open the user interface is `INSERT`.

You can change this key in the `ReturnOfModding/config/Hell1Modding-Hell1Modding-General.cfg` file, under the `[GUI]` section (`Toggle Key`, a Windows virtual-key code).

If the file isn't there, it will appear once you've launched the game at least once with the mod loader installed.

The same file also controls the overlay scale, whether the log console is shown, and which log levels reach the console and the log file.

While the interface is open the player is frozen the same way the game's own menus freeze it — you cannot move or attack, but the world keeps running.

> **ImGui callbacks registered by mods are disabled by default.**
> `gui.add_imgui`, `gui.add_always_draw_imgui` and `gui.add_to_menu_bar` run
> mod Lua on the render thread, and Hades 1 presents from a Forge worker
> thread while the game is running its own Lua elsewhere. Driving one
> `lua_State` from both corrupts it and crashes shortly into gameplay. Set
> `Mod Lua Gui Callbacks = true` under `[GUI]` only if you are developing
> against that API and understand the risk.

## Using existing (ModImporter) mods

Mods written for **ModImporter** work as-is. Put the mod folder in

```
<game>\Content\Mods\<ModName>\
```

exactly as you always have — its `modfile.txt` is read at startup and its
imports run at the same points ModImporter inserted them. **Nothing is written
to disk**: the game's own scripts are never modified, so there is no install
step, no backups to restore, and uninstalling is deleting the folder.

Mods load in `Load Priority` order, lowest first, after every plugin — so
ModUtil and anything else in `plugins/` is fully set up before a legacy mod
runs.

Two limits worth knowing:

- **`SJSON` directives are not applied yet.** A mod that merges into the game's
  `.sjson` data will load its Lua and log a warning about the part that was
  skipped.
- **Legacy mods share the game's globals.** Two mods that overwrite the same
  global still conflict, exactly as they did under ModImporter. Mods that go
  through ModUtil compose properly; that is what ModUtil is for.

Check the Mods tab in the overlay to see which were found, their priority, and
whether any reference a file that is missing. `Load Content Mods Folder` under
`[Mods]` in the config turns the whole thing off.

## Creating mods

- Define a `main.lua` file in which to code your mod.

- Create a `manifest.json` file that follows the Thunderstore Version 1 Manifest format.

- Create a folder whose name follows the GUID format `TeamName-ModName`, for example: `ReturnOfModding-DebugToolkit`.

- Place the `main.lua` file and the `manifest.json` file in the folder you've just created.

- Place the newly created folder in the `plugins` folder in the ReturnOfModding root folder, called `ReturnOfModding`, so the path to your manifest.json should be something like `ReturnOfModding/plugins/ReturnOfModding-DebugToolkit/manifest.json`.

Load order is resolved from each manifest's `dependencies`, so a mod always loads after the libraries it declares.

> **Porting a mod from Hades II?** Its manifest almost certainly declares a hard
> dependency on `Hell2Modding-Hell2Modding`. The loader refuses to load a mod
> whose declared dependency is missing, and that cascades through everything
> depending on it. Until Hades 1 variants are published, remove that one line
> from `manifest.json`.

## The mod API

Your `main.lua` runs inside the game's own Lua state, with a `rom` table
available.

| API | What it gives you |
|---|---|
| `rom.game` | the game's `_G` — every global the game's own scripts define |
| `rom.log` | `info` / `warning` / `error`, written to `LogOutput.log` and the console |
| `rom.mods` | other loaded mods, keyed by GUID |
| `rom.on_import.pre(fn)` | called before the game loads a script; may return an `_ENV` for it |
| `rom.on_import.post(fn)` | called after the game loads a script |
| `rom.paths` | `config`, `plugins`, `plugins_data`, `Content`, `Ship` |
| `rom.path` | path helpers, including `combine` |
| `rom.audio.load_bank(path)` | load a custom FMOD `.bank` |
| `rom.tolk` | screen reader output — `output`, `silence`, `detect_screen_reader`, `is_loaded` |

The standard Lua libraries are all present, including `require`, `package`,
`io` and `os` — the loader opens any the game leaves out. `lpeg` and the
LuaSocket family (`socket`, `mime`, `ltn12`, `socket.http`, `socket.url`) are
bundled and reachable through `require`.

A minimal mod:

```lua
rom.log.info("hello from my mod")

rom.on_import.post(function(script)
    if script == "RoomManager.lua" then
        rom.log.info("the game finished loading its scripts")
    end
end)
```

`on_import.pre` returning a table installs it as that script's `_ENV`, which is
how ModUtil injects itself. Chain to the real globals if you do this, or the
script's definitions will not reach the game:

```lua
rom.on_import.pre(function(script)
    if script ~= "RoomManager.lua" then return nil end
    return setmetatable({}, { __index = rom.game, __newindex = rom.game })
end)
```

Use the **table** form of `__index` / `__newindex`, not a closure. A closure
that writes `rom.game[k] = v` runs during the script's own compilation and
raises `table index is nil`; pointing both metamethods straight at `rom.game`
lets Lua do the redirect itself, with no intervening call.

Note the game loads its scripts **twice** during startup, so `on_import`
callbacks fire once per wave. Write them to be idempotent.

### Globals and saving

Hades 1 saves the game by walking `_G` and serialising **everything that is not
listed in `SaveIgnores`**. The type filter only applies at the top level, so a
table survives it and the serialiser then recurses into the whole thing. One
function or userdata anywhere inside kills the save with

```
Script Crash: Main.lua:1035 unsupported type detected
```

Most mods never hit this, because ENVY gives each mod its own `_ENV` and
ModUtil works through a proxy — neither writes to the real `_G`. But if your
mod deliberately publishes a global that holds functions, register it once the
game has defined `SaveIgnores`:

```lua
rom.on_import.post(function(script)
    if script == "Main.lua" then
        rom.game.SaveIgnores.MyModGlobal = true
    end
end)
```

The loader does exactly this for `rom` itself, and ModUtil does it for mods
registered through it. This is the same mechanism Supergiant use for
`package`, `io`, `os` and the other standard libraries.

Note that Hades 2 saves by *whitelist* instead, so a mod ported from there
will not have needed this.

### `modfile.txt` load order

`SGG_Modding-DemonDaemon` reads a mod's `modfile.txt` at runtime, which is how
the old ModImporter format still works — `Top Import` runs before the target
script, `Import` after it, and nothing is written to disk.

**Set the target explicitly:**

```
To "Scripts/Main.lua"
```

The default target is `RoomLogic.lua`, which is Hades 2's name for it. A mod
that omits the `To` line queues its imports against a script that never loads
in Hades 1, so the mod appears to load fine and then does nothing at all.

### Patching the game's own scripts

For changes that have to happen *inside* a game script rather than around it,
ship a [lovely](https://github.com/ethangreen-dev/lovely-injector) patch file
as `plugins/<Namespace>-<Mod>/lovely.toml`, or several under
`plugins/<Namespace>-<Mod>/lovely/*.toml`. The script's text is rewritten on
its way into the compiler; nothing is written to disk.

```toml
[manifest]
version = "1.0.0"
priority = 0

[[patches]]
[patches.pattern]
target = "roommanager.lua"
pattern = 'Import "Color.lua"'
position = "after"
payload = 'MyMod_Loaded = true'
match_indent = false
times = 1
```

`target` is the script's filename, lowercased — `RoomManager.lua` is
`roommanager.lua`. Pattern, regex and copy patches are all supported; see
lovely's own documentation for the full schema.

Every patch file loaded, and every patch applied, is reported in
`ReturnOfModding/lovely.log`. A patched script is also dumped to
`ReturnOfModding/lovely_dump/` so you can read what the game actually
compiled.

Drop a `.lovelyignore` file in a mod's folder to skip its patches without
uninstalling it.

---

## Folder Convention

The `subfolder` we are referring to in the paragraphs below always follows the GUID format `TeamName-ModName`, for example: `ReturnOfModding-DebugToolkit`.

It also refers to the mod `subfolder` in its respective root folder.

Copying folders must preserve the folder hierarchy without flattening it.

### `plugins`

Location of `.lua`, `README`, and `manifest.json`.
 
#### First Installation
- Copy content into `plugins/subfolder`.

#### Update
- Erase `plugins/subfolder` directory if it already exists.
- Copy content into `plugins/subfolder` directory.

#### Uninstallation
- Delete `plugins/subfolder` directory.
  
### `plugins_data`

This directory is primarily used for data that must persist between sessions and should not be altered by the user.

To ensure the mod manager does not modify certain data during updates, use a folder called `cache` located at `plugins_data/subfolder/cache`.

#### First Installation
- Copy the contents into the `plugins_data/subfolder` directory.

#### Update
- If the `plugins_data/subfolder/cache` folder does not exist, delete the entire `plugins_data/subfolder` directory. Otherwise, delete everything in the directory except the `cache` folder.
- Copy the new contents into the `plugins_data/subfolder` directory, overwriting any existing files with the same names, including those in the `plugins_data/cache` folder if present in their .zip package.

#### Uninstallation
- If the `plugins_data/subfolder/cache` folder does not exist, delete the entire `plugins_data/subfolder` directory. Otherwise, delete everything in the directory except the `cache` folder.

### `config`

This directory is primarily used for data that must persist between sessions and can be manipulated by the user.

Mod creators are advised not to include pre-created `.cfg` files in their `config` folder within their `.zip` package, as this will cause the mod manager to overwrite any existing user configuration files with the same name (e.g., during updates).

Instead, let your code generate `.cfg` files using the mod API `config.config_file`.

A valid use case for providing pre-made configuration files is when creating a modpack that aims to deliver a curated, set-in-stone experience.

#### First Installation
- Copy the contents into the `config/subfolder` directory.

#### Update
- Copy the contents into the `config/subfolder` directory, overwriting any existing files with the same names.

#### Uninstallation
- Do nothing.

## Mod Manager Integration

If you'd like to integrate Hell1Modding into your mod manager, here are the specifications:

- Hell1Modding is injected into the game process using DLL hijacking, which is the same technique used by other bootstrappers such as [UnityDoorstop](https://github.com/NeighTools/UnityDoorstop).

  The dll used by this technique depends on the game you're using the mod loader for. This means that the name of the dll may change from one game to another. **For Hades it is `d3d11.dll`, and it belongs in the `x64` folder alongside `Hades.exe`.**

- The root folder used by Hell1Modding (which will then be used to load mods from this folder) can be defined in several ways:

  - Setting the process environment variable: `rom_modding_root_folder <CUSTOM_PATH>`

  - Command line argument when launching the game executable: `--rom_modding_root_folder <CUSTOM_PATH>`

  - If the process environment variable is not defined, the command line arguments are checked. If neither is defined, the ReturnOfModding folder is placed in the game folder, next to the game executable.

- The loader can be switched on and off explicitly with the `rom_enabled` environment variable or command line argument (`true` enables it).

- The root folder is always named `ReturnOfModding`.

- See [Folder Convention](#folder-convention) to find out how to manage mod installation, update, and uninstallation.

---

## Building

Requires MSVC, CMake 3.24+, and a [Rust toolchain](https://rustup.rs)
with the `x86_64-pc-windows-msvc` target — lovely-lib is a Rust staticlib and
is the one dependency that is not C or C++. Everything else is fetched by
CMake.

```
cmake -B build -A x64
cmake --build build --config Release
```

The output is `build/Release/d3d11_.dll`; rename it to `d3d11.dll` when
deploying. **Release only** — a Debug build breaks the `eastl::string` ABI the
game expects and the proxy's tail-call stubs.

## Credits

- **xiaoxiao921** for [ReturnOfModdingBase](https://github.com/xiaoxiao921/ReturnOfModdingBase),
  which this is a fork of, and for
  [Hell2Modding](https://github.com/SGG-Modding/Hell2Modding), which showed how
  a Supergiant game fits the framework.
- **SGG Modding** for ModUtil and the surrounding library ecosystem.
- **Supergiant Games**, who ship full PDB symbols with Hades. This project
  would have been dramatically harder without them.

## License

MIT. See [LICENSE](LICENSE).
