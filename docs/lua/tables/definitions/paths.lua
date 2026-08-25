---@meta paths

-- Table containing helpers for retrieving project related IO file/folder paths.
---@class (exact) paths

-- Used for data that must persist between sessions and that can be manipulated by the user.
---@return string # Returns the config folder path
function paths.config() end

-- Used for data that must persist between sessions but not be manipulated by the user.
---@return string # Returns the plugins_data folder path
function paths.plugins_data() end

-- Location of .lua, README, manifest.json files.
---@return string # Returns the plugins folder path
function paths.plugins() end

-- The game's Content folder, which holds Scripts, Game, Maps, Audio and so on.
---@return string # absolute path to <game>/Content
function paths.Content() end

-- The folder the executable lives in. Note Hades 1 has no folder actually
--named "Ship" - the layout is Content/, x64/, x64Vk/, x86/ - so this
--returns x64/. The name is kept for parity with Hades II.
---@return string # absolute path to <game>/x64
function paths.Ship() end


