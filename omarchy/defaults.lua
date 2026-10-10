-- Applied by the Omarchy plugin after Hymission loads and after every config
-- reload. Copy this file and omarchy/bindings.json into
-- ~/.config/hymission/omarchy.lua to take full control of both.
if not (hl.plugin and hl.plugin.hymission) then return end

-- Three-finger vertical swipe opens and closes the overview with live progress.
hl.plugin.hymission.gesture({ fingers = 3, direction = "vertical", action = "toggle", args = "forceall" })
