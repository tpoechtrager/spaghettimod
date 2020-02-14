--[[

  Rugby mode: pass the flag you hold to a teammate by shooting at him with rifle
  Addition for CTF: Make the flagholder wait a certain amount of time before passing becomes possible. This is to prevent lazy scoring.

]]--

local playermsg = require"std.playermsg"

local module = {}

local passdelay = 4000
local function timeremaining(ci)
  local pickup = ci.extra.flagpickup
  if not pickup then return false end
  local remaining = passdelay - (engine.totalmillis - pickup)
  if remaining > 0 then
    return tonumber(string.format("%.0f", remaining / 1000))
  else
    ci.extra.flagpickup = nil
    return false
  end
end

local function allowpass(actor)
  if server.m_hold or server.m_protect then return true end -- only restrict passing in ctf
  if timeremaining(actor) then 
    local time = timeremaining(actor)
    local plural = (time ~= 1) and "s" or ""
    playermsg("\f6Info\f7: Passing is \f3delayed\f7! You can pass the flag in \f3" .. time .. "\f7 second" .. plural .. "!", actor)
    return false
  else
    return true
  end
end

local dodamagehook
function module.on(state)
  if dodamagehook then spaghetti.removehook(dodamagehook) dodamagehook = nil end
  if not state then return end
  dodamagehook = spaghetti.addhook("dodamage", function(info)
    if info.skip or not server.m_ctf or info.target.team ~= info.actor.team or info.gun ~= server.GUN_RIFLE then return end
    local flags, actorflags = server.ctfmode.flags, {}
    for i = 0, flags:length() - 1 do if flags[i].owner == info.actor.clientnum then actorflags[i] = true end end
    if not next(actorflags) then return end
    info.skip = true
    if not allowpass(info.actor) then return end
    for flag in pairs(actorflags) do
      server.ctfmode:returnflag(flag, 0)
      server.ctfmode:takeflag(info.target, flag, flags[flag].version)
    end
    if info.actor.extra.flagpickup then info.actor.extra.flagpickup = nil end
    local hooks = spaghetti.hooks.rugbypass
    if hooks then hooks{ actor = info.actor, target = info.target, flags = actorflags } end
  end)
end

spaghetti.addhook("takeflag", function(info) 
  info.ci.extra.flagpickup = engine.totalmillis
end)

spaghetti.addhook("dropflag", function(info) 
  info.ci.extra.flagpickup = nil
end)

return module

