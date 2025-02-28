-- mod-version:2 -- lite-xl 2.0
local core = require "core"
local common = require "core.common"
local command = require "core.command"
local config = require "core.config"
local keymap = require "core.keymap"
local style = require "core.style"
local RootView = require "core.rootview"
local CommandView = require "core.commandview"

config.plugins.scale = common.merge({
  mode = "code",
  use_mousewheel = true
}, config.plugins.scale)

local scale_steps = 0.05

local current_scale = SCALE
local default_scale = SCALE

local function set_scale(scale)
  scale = common.clamp(scale, 0.2, 6)

  -- save scroll positions
  local scrolls = {}
  for _, view in ipairs(core.root_view.root_node:get_children()) do
    local n = view:get_scrollable_size()
    if n ~= math.huge and not view:is(CommandView) and n > view.size.y then
      scrolls[view] = view.scroll.y / (n - view.size.y)
    end
  end

  local s = scale / current_scale
  current_scale = scale

  if config.plugins.scale.mode == "ui" then
    SCALE = scale

    style.padding.x      = style.padding.x      * s
    style.padding.y      = style.padding.y      * s
    style.divider_size   = style.divider_size   * s
    style.scrollbar_size = style.scrollbar_size * s
    style.caret_width    = style.caret_width    * s
    style.tab_width      = style.tab_width      * s

    for _, name in ipairs {"font", "big_font", "icon_font", "icon_big_font", "code_font"} do
      style[name] = renderer.font.copy(style[name], s * style[name]:get_size())
    end
  else
    style.code_font = renderer.font.copy(style.code_font, s * style.code_font:get_size())
  end

  for _, font in pairs(style.syntax_fonts) do
    renderer.font.set_size(font, s * font:get_size())
  end

  for _, font in pairs(style.syntax_fonts) do
    renderer.font.set_size(font, s * font:get_size())
  end

  -- restore scroll positions
  for view, n in pairs(scrolls) do
    view.scroll.y = n * (view:get_scrollable_size() - view.size.y)
    view.scroll.to.y = view.scroll.y
  end

  core.redraw = true
end

local function get_scale()
  return current_scale
end

local function res_scale()
  set_scale(default_scale)
end

local function inc_scale()
  set_scale(current_scale + scale_steps)
end

local function dec_scale()
  set_scale(current_scale - scale_steps)
end


command.add(nil, {
  ["scale:reset"   ] = function() res_scale() end,
  ["scale:decrease"] = function() dec_scale() end,
  ["scale:increase"] = function() inc_scale() end,
})

keymap.add {
  ["ctrl+0"] = "scale:reset",
  ["ctrl+-"] = "scale:decrease",
  ["ctrl+="] = "scale:increase",
  ["ctrl+wheelup"] = "scale:increase",
  ["ctrl+wheeldown"] = "scale:decrease"
}

return {
  ["set"] = set_scale,
  ["get"] = get_scale,
  ["increase"] = inc_scale,
  ["decrease"] = dec_scale,
  ["reset"] = res_scale
}
