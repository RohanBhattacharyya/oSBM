function patch(config)
  config.osbmMobileHud = true
  config.statusIconPos = {33, 72}
  local bar = config.mainBar
  bar.size = {80, 126}
  local positions = {questLog = {0, 84}, inventory = {40, 84}, codex = {0, 42}, beam = {40, 42}, deploy = {40, 0}}
  for name, button in pairs(bar) do
    if type(button) == "table" and name ~= "size" then
      if positions[name] then
        local p = positions[name]
        button.pos = p
        button.poly = {{p[1], p[2]}, {p[1] + 34, p[2]}, {p[1] + 34, p[2] + 38}, {p[1], p[2] + 38}}
      elseif button.pos then
        button.pos = {-100000, -100000}
        button.poly = jarray()
      end
      for key, value in pairs(button) do
        if type(value) == "string" and string.sub(value, 1, 1) == "/" and key ~= "scriptPane" then
          button[key] = value .. "?scalenearest=2"
        end
      end
    end
  end
  for _, key in ipairs({"base", "hover", "open", "openHover"}) do
    bar.codex[key] = "/controls/buttonempty (" .. (key == "base" and 1 or 2) .. ").png?scalenearest=2"
  end
  config.cursorTooltip.codexText = "Quick menu"
  return config
end
