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

To uninstall, delete `d3d11.dll`. Nothing else is modified.

---

## User Interface

Comes with an ImGui user interface.

The default key to open the user interface is `INSERT`.

You can change this key in the `ReturnOfModding/config/Hell1Modding-Hell1Modding-General.cfg` file, under the `[GUI]` section (`Toggle Key`, a Windows virtual-key code).

If the file isn't there, it will appear once you've launched the game at least once with the mod loader installed.

The same file also controls the overlay scale, whether the log console is shown, and which log levels reach the console and the log file.

While the interface is open the player is frozen the same way the game's own menus freeze it — you cannot move or attack, but the world keeps running.

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
    return setmetatable({}, {
        __index    = rom.game,
        __newindex = function(_, k, v) rom.game[k] = v end,
    })
end)
```

Note the game loads its scripts **twice** during startup, so `on_import`
callbacks fire once per wave. Write them to be idempotent.

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

Requires MSVC and CMake 3.24+. Dependencies are fetched by CMake.

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
