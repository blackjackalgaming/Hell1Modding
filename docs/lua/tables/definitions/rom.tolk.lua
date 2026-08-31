---@meta tolk

-- Screen reader output, for mods that speak UI text aloud.
--
--The driver is loaded lazily on the first call. Hades 1 ships none of the
--screen reader client DLLs, so NVDA / JAWS / System Access are only
--detected if their own software is installed and running; otherwise Tolk
--falls back to SAPI.
---@class (exact) rom.tolk

-- Outputs text through the current screen reader driver.
---@param str string The text to speak.
---@return boolean # true if the text was handed to a driver.
function tolk.output(str) end

-- Stops whatever the screen reader is currently saying.
---@return boolean # true if the driver acknowledged the request.
function tolk.silence() end

-- Reports which screen reader Tolk is talking to.
---@return string # The name of the active driver, or nil if none is running.
function tolk.detect_screen_reader() end

-- Whether the screen reader layer is up. Calling any other tolk function
--loads it, so this is only false before the first use.
---@return boolean # true once a driver has been loaded.
function tolk.is_loaded() end


