---@meta audio

-- Audio helpers. Reaches FMOD Studio directly rather than going through the
--engine's own wrapper, so it uses FMOD's documented public API.
---@class (exact) rom.audio

-- Loads an FMOD Studio bank into the game's audio system. The game's own
--banks live in Content/Audio/FMOD/Build/Desktop; a mod's can live
--anywhere, since this takes a full path.
---@param file_path string absolute path to an FMOD .bank file
---@return bool # true if FMOD accepted the bank.
function audio.load_bank(file_path) end


