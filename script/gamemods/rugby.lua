--[[

  Rugby mode: pass the flag you hold to a teammate by shooting at him with rifle
  Addition for CTF: Set a limit on the pass range and dynamically highlight teammates that the flagholder can still pass to.

  Warning: This mod might cause higher-than-normal CPU usage.

]]--

local ents, playermsg, commands, iterators = require"std.ents", require"std.playermsg", require"std.commands", require"std.iterators"
local n_client, putf = require"std.n_client", require"std.putf"
local fp, L, vec3  = require"utils.fp", require"utils.lambda", require"utils.vec3"
local map = fp.map
require"std.notalive"

local module, hooks, highlights = {}, {}, false


--[[
    Particle effects config
]]

module.effects = { 
  teammates = {
    z = 0,        -- add to z
    particle = {
      a1 = 11,    -- particle type; I use flames so the following attributes are:
      a2 = 50,    -- radius
      a3 = 50,    -- height
      a4 = 0x060, -- color
      a5 = 0      -- unused
    }
  },
  flagholder = {
    z = 16, 
    particle = {
      a1 = 11, 
      a2 = 50, 
      a3 = 50, 
      a4 = 0x620,
      a5 = 0 
    }
  }
}


--[[ 
    Pass range restriction 
]]

local basedist, share = 1500, 2/5

local function specialmap()
  return (server.smapname == "recovery" or server.smapname == "mercury" or server.smapname == "l_ctf") and 1500 or nil
end

local function calcmapsize() 
  if not server.m_ctf or server.m_hold or server.m_protect then return end
  local base1, base2 = nil, nil
  for i, _, ment in ents.enum(server.FLAG) do
    if base1 then base2 = ment.o else base1 = ment.o end
  end
  if not base1 or not base2 then return end
  basedist = specialmap() or (vec3(base1):dist(base2) + 300)
  base1, base2 = nil, nil
end

local function inrange(actor, target)
  return vec3(actor.state.o):dist(target.state.o) <= (basedist * share)
end

local function valident(ent)
  return 
end


--[[ 
    Particle effects: Highlight all teammates in pass-range for the flagholder. Also indicate to teammates if the flagholder can pass to them. 
    Assign a placeholder particle to every client on connect and have a spaghetti.later send N_EDITENT packets to the flagholder and mates 
    to update its position/appearance on the client side. Somewhat like std.trackent but better suited for this application.
]] 

local function stophlmate(ci, mate)
  if not highlights then return end
  if ci.state.aitype == server.AI_BOT then return end
  local i, _, ment = ents.getent(mate.extra.particle)
  if not i or ment.type ~= server.NOTUSED then return end 
  local p = n_client(putf({ 20, r = 1}, server.N_EDITENT, i, -1e7,  -1e7,  -1e7, server.NOTUSED, 0, 0, 0, 0, 0), ci):finalize()
  engine.sendpacket(ci.clientnum, 1, p, -1)
end

local function starthlmate(ci, mate, isflagholder)
  if not highlights or not (ci.team == mate.team) then stophlmate(ci, mate) return end
  if ci.state.aitype == server.AI_BOT then return end
  local effect = isflagholder and module.effects.flagholder or module.effects.teammates 
  local pe, o, i, _, ment = effect.particle, vec3(mate.state.o), ents.getent(mate.extra.particle)
  if not i or not o or ment.type ~= server.NOTUSED then return end
  o.z = o.z + effect.z
  local p = n_client(putf({r = true}, server.N_EDITENT, i, o.x * server.DMF, o.y * server.DMF, o.z * server.DMF, server.PARTICLES, pe.a1, pe.a2, pe.a3, pe.a4, pe.a5), ci):finalize()
  engine.sendpacket(ci.clientnum, 1, p, -1)
end

local function starthighlights(ci)
  if not highlights then return end
  ci.extra.highlightlater = spaghetti.later(100, function() 
    for p in iterators.all() do
      stophlmate(ci, p)
      stophlmate(p, ci)
      if (p.state.state == engine.CS_ALIVE) and inrange(ci, p) and (ci.clientnum ~= p.clientnum) and (ci.team == p.team) then
        starthlmate(ci, p)
        starthlmate(p, ci, true)
      end
    end
  end, true)
end

local function stophighlights(ci, notall)
  local token, hadtoken = ci.extra.highlightlater, false
  if token then 
    spaghetti.cancel(token) 
    hadtoken = true
  end
  if hadtoken or (not hadtoken and not notall) then
    for p in iterators.inteam(ci.team) do
      stophlmate(ci, p)
      stophlmate(p, ci)
    end
  end
  ci.extra.highlightlater = nil
end

commands.add("hltoggle", function(info)
  if info.ci.privilege < server.PRIV_ADMIN then return playermsg("Access denied.", info.ci) end  
  if highlights then for p in iterators.all() do stophighlights(p) end end
  highlights = not highlights
  playermsg("\f6Info\f7: Radius highlights are " .. (highlights and "enabled" or "disabled") .. " from now on.", info.ci)
end)


--[[ 
    Rugby implementation 
]]


function module.on(state)
  map.np(L"spaghetti.removehook(_2)", hooks)
  if not state then return end
  highlights = state
  hooks = {}
  hooks.dodamagehook = spaghetti.addhook("dodamage", function(info)
    if info.skip or not server.m_ctf or info.target.team ~= info.actor.team or info.gun ~= server.GUN_RIFLE then return end
    local flags, actorflags = server.ctfmode.flags, {}
    for i = 0, flags:length() - 1 do if flags[i].owner == info.actor.clientnum then actorflags[i] = true end end
    if not next(actorflags) then return end
    info.skip = true
    if not inrange(info.actor, info.target) then 
      return playermsg("\f6Info\f7: You can only pass to teammates \f6close to you\f7! Close teammates have a \f0green indicator\f7!", info.actor)
    end
    for flag in pairs(actorflags) do
      server.ctfmode:returnflag(flag, 0)
      server.ctfmode:takeflag(info.target, flag, flags[flag].version) 
    end
    stophighlights(info.actor) 
    local hooks = spaghetti.hooks.rugbypass
    if hooks then hooks{ actor = info.actor, target = info.target, flags = actorflags } end
  end, true)
  hooks.entshook = spaghetti.addhook("entsloaded", function(info) 
    calcmapsize()
  end)
  hooks.connecthook = spaghetti.addhook("connected", function(info) 
    info.ci.extra.particle = ents.newent(server.NOTUSED, nil, 0, 0, 0, 0, 0, L"")
  end)
  hooks.disconnecthook = spaghetti.addhook("clientdisconnect", function(info) 
    stophighlights(info.ci)
    if info.ci.extra.particle then ents.delent(info.ci.extra.particle) end -- delete ent entirely
  end)
  hooks.botjoin = spaghetti.addhook("botjoin", function(info) 
    info.ci.extra.particle = ents.newent(server.NOTUSED, nil, 0, 0, 0, 0, 0, L"")
  end)
  hooks.botleave = spaghetti.addhook("botleave", function(info) 
    stophighlights(info.ci)
    if info.ci.extra.particle then ents.delent(info.ci.extra.particle) end
  end)
  hooks.diedhook = spaghetti.addhook("notalive", function(info) 
    stophighlights(info.ci, true)
  end)
  hooks.switchteam = spaghetti.addhook(server.N_SWITCHTEAM, function(info) 
    stophighlights(info.ci)
  end)
  hooks.setteam = spaghetti.addhook(server.N_SETTEAM, function(info) 
    stophighlights(engine.getclientinfo(info.who))
  end)
  hooks.takeflag = spaghetti.addhook("takeflag", function(info) 
    stophighlights(info.ci)
    starthighlights(info.ci)
  end)
  hooks.dropflag = spaghetti.addhook("dropflag", function(info) 
    stophighlights(info.ci)
  end)
  hooks.scoreflag = spaghetti.addhook("scoreflag", function(info) 
    stophighlights(info.ci)
  end)
  hooks.changemap = spaghetti.addhook("changemap", function(info) 
    for p in iterators.all() do stophighlights(p, true) end
  end)
  hooks.changemap = spaghetti.addhook("changemap", function(info) 
    for p in iterators.all() do 
      stophighlights(p, true) 
    end
  end)
  hooks.maploaded = spaghetti.addhook("maploaded", function(info) 
    if info.ci.extra.particle then 
      ents.delent(info.ci.extra.particle) 
      info.ci.extra.particle = nil
    end 
    info.ci.extra.particle = ents.newent(server.NOTUSED, nil, 0, 0, 0, 0, 0, L"")
  end)
end

return module

