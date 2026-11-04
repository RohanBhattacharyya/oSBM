-- Run from the repository root with Lua 5.3. Uses the real UI script with
-- fake engine callbacks; no assets, audio device or game startup required.
local function exercise(settings, unavailable)
  local dismissals, draws, nextItem = 0, 0, 0
  local function draw() draws = draws + 1 end
  local canvas = setmetatable({}, {__index = function() return draw end})
  local env = setmetatable({
    root = {assetJson = function() return {} end},
    pane = {scale = function() return 1 end,
      dismiss = function() dismissals = dismissals + 1 end},
    voice = {getSettings = function() return settings end,
      devices = function() return {} end,
      speaker = function() return {smoothDecibels = -80} end},
    widget = setmetatable({
      bindCanvas = function() return canvas end,
      addListItem = function() nextItem = nextItem + 1; return tostring(nextItem) end
    }, {__index = function() return draw end})
  }, {__index = _G})
  assert(loadfile("assets/opensb/interface/opensb/voicechat/voicechat.lua", "t", env))()
  env.init()
  env.displayed()
  env.update()
  if unavailable then
    assert(dismissals == 1 and draws == 0, "unavailable voice must dismiss without drawing missing settings")
  else
    assert(dismissals == 0 and draws > 0, "available voice must keep drawing normally")
  end
end

exercise({}, true)
exercise({inputMode = false}, true)
exercise({inputMode = 7}, true)
for _, mode in ipairs({"PushToTalk", "VoiceActivity"}) do
  exercise({enabled = true, inputEnabled = false, inputMode = mode,
    threshold = -50, inputVolume = 1, outputVolume = 1}, false)
end
print("Passed 5 voice panel lifecycle checks")
