# Table: paths

Table containing helpers for retrieving project related IO file/folder paths.

## Functions (5)

### `config()`

Used for data that must persist between sessions and that can be manipulated by the user.

- **Returns:**
  - `string`: Returns the config folder path

**Example Usage:**
```lua
string = paths.config()
```

### `plugins_data()`

Used for data that must persist between sessions but not be manipulated by the user.

- **Returns:**
  - `string`: Returns the plugins_data folder path

**Example Usage:**
```lua
string = paths.plugins_data()
```

### `plugins()`

Location of .lua, README, manifest.json files.

- **Returns:**
  - `string`: Returns the plugins folder path

**Example Usage:**
```lua
string = paths.plugins()
```

### `Content()`

The game's Content folder, which holds Scripts, Game, Maps, Audio and so on.

- **Returns:**
  - `string`: absolute path to <game>/Content

**Example Usage:**
```lua
string = paths.Content()
```

### `Ship()`

The folder the executable lives in. Note Hades 1 has no folder actually
named "Ship" - the layout is Content/, x64/, x64Vk/, x86/ - so this
returns x64/. The name is kept for parity with Hades II.

- **Returns:**
  - `string`: absolute path to <game>/x64

**Example Usage:**
```lua
string = paths.Ship()
```


