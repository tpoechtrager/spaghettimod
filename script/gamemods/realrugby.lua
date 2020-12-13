--[[

  Rugby mode: pass the flag you hold to a teammate by shooting at him with rifle
  Addition for CTF: Only allow tactical side and backwards passes to prevent open maps from being finished too early through effortless long-range passes.

]]--

local ents, vec3, playermsg = require"std.ents", require"utils.vec3", require"std.playermsg"
local module = {}

local dodamagehook
local basedist

local function specialmap()
  return server.smapname == "recovery" and 1000 or server.smapname == "mercury" and 1500 or nil
end

spaghetti.addhook("entsloaded", function(info) 
  local bases = {}
  local base1, base2 = nil, nil
  for i, _, ment in ents.enum(server.FLAG) do
    if base1 then base2 = ment.o else base1 = ment.o end
  end
  if not base1 or not base2 then return end
  local basedist = specialmap() or vec3(base1):dist(base2)
  base1, base2 = nil, nil
  server.sendservmsg("Base distance: " .. basedist)
end)

local tolerance = 60 -- add a slight tolerance
local function allowpass(actor, target)
  if server.m_hold or server.m_protect then return true end -- only restrict passing in ctf
  for i, _, ment in ents.enum(server.FLAG) do
    local flagteam = server.ctfflagteam(ment.attr2)
    if actor.team == flagteam then
      local dist1, dist2 = vec3(ment.o):dist(actor.state.o), vec3(ment.o):dist(target.state.o)
      if dist1 <= (dist2 + tolerance) then 
        return true 
      end
    end
  end
  playermsg("\f6Info\f7: You can only do \f6tactical side or backwards passes\f7! This is actual Rugby, you still gotta \f6run\f7!!", actor)
  return false
end

function module.on(state)
  if dodamagehook then spaghetti.removehook(dodamagehook) dodamagehook = nil end
  if not state then return end
  dodamagehook = spaghetti.addhook("dodamage", function(info)
    if info.skip or not server.m_ctf or info.target.team ~= info.actor.team or info.gun ~= server.GUN_RIFLE then return end
    local flags, actorflags = server.ctfmode.flags, {}
    for i = 0, flags:length() - 1 do if flags[i].owner == info.actor.clientnum then actorflags[i] = true end end
    if not next(actorflags) then return end
    info.skip = true
    if not allowpass(info.actor, info.target) then return end
    for flag in pairs(actorflags) do
      server.ctfmode:returnflag(flag, 0)
      server.ctfmode:takeflag(info.target, flag, flags[flag].version)
    end
    local hooks = spaghetti.hooks.rugbypass
    if hooks then hooks{ actor = info.actor, target = info.target, flags = actorflags } end
  end)
end

return module

