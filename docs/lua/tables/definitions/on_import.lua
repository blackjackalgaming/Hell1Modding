---@meta on_import

-- Callbacks fired around each script the game imports from the game's
--Content/Scripts folder. Required by ModUtil 4.0.1.
---@class (exact) on_import

-- Called before the game loads a script from Content/Scripts. Returning a
--table installs it as that script's _ENV, which is how ModUtil injects
--itself. Chain to rom.game via __index/__newindex or the script's globals
--will not reach the game.
---@param callback function signature (string script_name, current_ENV) returning nil or an _ENV
function on_import.pre(callback) end

-- Called after the game has loaded a script from Content/Scripts. Note the
--game loads its scripts twice at startup, so this fires once per wave.
---@param callback function signature (string script_name)
function on_import.post(callback) end


