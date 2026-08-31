# Table: rom.tolk

Screen reader output, for mods that speak UI text aloud.

The driver is loaded lazily on the first call. Hades 1 ships none of the
screen reader client DLLs, so NVDA / JAWS / System Access are only
detected if their own software is installed and running; otherwise Tolk
falls back to SAPI.

## Functions (4)

### `output(str)`

Outputs text through the current screen reader driver.

- **Parameters:**
  - `str` (string): The text to speak.

- **Returns:**
  - `boolean`: true if the text was handed to a driver.

**Example Usage:**
```lua
boolean = rom.tolk.output(str)
```

### `silence()`

Stops whatever the screen reader is currently saying.

- **Returns:**
  - `boolean`: true if the driver acknowledged the request.

**Example Usage:**
```lua
boolean = rom.tolk.silence()
```

### `detect_screen_reader()`

Reports which screen reader Tolk is talking to.

- **Returns:**
  - `string`: The name of the active driver, or nil if none is running.

**Example Usage:**
```lua
string = rom.tolk.detect_screen_reader()
```

### `is_loaded()`

Whether the screen reader layer is up. Calling any other tolk function
loads it, so this is only false before the first use.

- **Returns:**
  - `boolean`: true once a driver has been loaded.

**Example Usage:**
```lua
boolean = rom.tolk.is_loaded()
```


