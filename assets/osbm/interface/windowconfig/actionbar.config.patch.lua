-- Asset patch sandboxes have no require(); keep this helper local.
-- Enlarge the HUD in asset space; inventory windows keep the user's UI scale.
local imageKeys = {file=true, fileBody=true, fileHeader=true, fileFooter=true,
  base=true, hover=true, pressed=true, baseImageChecked=true, backingImage=true,
  background=true, inner=true, begin=true, ["end"]=true}
local vectorKeys = {position=true, size=true, pressedOffset=true}
local function scaleHud(value, factor)
  if type(value) ~= "table" then return end
  for key, child in pairs(value) do
    if imageKeys[key] and type(child) == "string" and child:sub(1,1) == "/" then
      value[key] = child .. "?scalenearest=" .. factor
    elseif vectorKeys[key] and type(child) == "table" and #child == 2 then
      value[key] = {math.floor(child[1] * factor + 0.5), math.floor(child[2] * factor + 0.5)}
    elseif key == "fontSize" and type(child) == "number" then
      value[key] = math.floor(child * factor + 0.5)
    else
      scaleHud(child, factor)
    end
  end
  if value.type == "portrait" then value.scale = (value.scale or 1) * factor end
  if value.type == "itemslot" then value.itemScale = (value.itemScale or 1) * factor end
end

function patch(config)
  local factor = 1.25
  scaleHud(config.paneLayout, factor)
  for _, key in ipairs({"actionBarSelectOffset", "countMidAnchor"}) do
    local p = config[key]
    config[key] = {math.floor(p[1] * factor + 0.5), math.floor(p[2] * factor + 0.5)}
  end
  config.osbmResponsive = true
  return config
end
