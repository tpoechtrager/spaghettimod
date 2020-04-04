--[[
    Spawn some custom items for the Rigatoni easter special and use carrots as default easter eggs.
    Keep egg appearance and effect mapping in configurable table 'easterents'. Count collected easter eggs and persist map-specific highscores.
    
    Use #editeggs to enter and exit egg edit mode, and then #addegg and #removeegg to create and delete eggs at the player position.
    Changes in the egg rotation take effect immediately.
    All coordinates are saved to one single file, which at default is var/coordinates.

    Config + some setup examples starting at line 180.
]]

local playermsg, ents, sound, hitpush, trackent = require"std.playermsg", require"std.ents", require"std.sound", require"std.hitpush", require"std.trackent"
local commands, iterators, putf, n_client = require"std.commands", require"std.iterators", require"std.putf", require"std.n_client"
local jsonpersist, servertag, vec3, fp, L = require"utils.jsonpersist", require"utils.servertag", require"utils.vec3", require"utils.fp", require"utils.lambda"
local map = fp.map

require"std.lastpos"
require"std.notalive"

-- db and utils
local eggfile = "./var/coordinates"

local f = io.open(eggfile)
if not f then return engine.writelog("Easter error: Cannot find file with coordinates, module disabled.")
else f:close() end

local enable, easteredit, easter, currentents, jumpers, invisibles = true, false, {}, {}, {}, {}

-- item effects
local extrahealth, higherjump, invisible, extrafrags, dizzy 
local spectators, emptypos = {}, {buf = ('\0'):rep(13)}

-- utils
local function hasmmreset()
  local cfg = io.open("packages/base/" .. server.smapname .. ".cfg")
  if not cfg then return false end
  for line in cfg:lines() do
    if line:match"mapmodelreset" then return true end
  end
  return false
end

local function fireball(o)
  local o = vec3(o)
  o.z = o.z + 10
  local p = ents.active() and ents.newent(server.PARTICLES, o, 3, 10, math.random(0, 0xFFF))
  if p then spaghetti.latergame(200, function() ents.delent(p) end) end
end

local function sparkle(egg)
  local o = egg.o
  egg.sparkle = spaghetti.latergame(200, function()
    local spark = ents.newent(
      server.PARTICLES, 
      { 
        x = o.x + tonumber(math.random() * 6 - 3), 
        y = o.y + tonumber(math.random() * 6 - 3), 
        z = o.z + 7 + tonumber(math.random() * 6 - 3)
      }, 
      4,                     -- streak effect
      309,                   -- circular inverted
      1,                     -- short length
      math.random(0, 0xFFF)) -- random color 
    spaghetti.later(200, function() ents.delent(spark) end) -- short lifespan
  end, true)
end

local function updatehealthbar(ci)
  if not ci.extra.healthbar then return end
  local share = ci.state.health / ci.state.maxhealth * 100
  trackent.remove(ci, ci.extra.healthbar)
  ci.extra.healthbar = nil
  if share <= 0 then return end
  ci.extra.healthbar = trackent.add(ci, function(i, lastpos)
    local o = vec3(lastpos.pos)
    o.z = o.z + 22
    ents.editent(i, server.PARTICLES, o, 5, share, 0x3F0)
  end, false, true, nil)
end

-- effect callbacks
extrahealth = function(ci)
  if ci.state.aitype ~= server.AI_NONE then return end
  local st = ci.state
  if st.state ~= engine.CS_ALIVE then return end
  st.maxhealth = 400
  st.health = math.min(st.health + 400, st.maxhealth)
  server.sendresume(ci)
  playermsg("\f6Info\f7: \f0QUAD HEALTH\f7: YOUR HEALTH IS NOW \f0" .. st.health .. "HP!! \f7THIS MAKES YOU (almost) INVINCIBLE!", ci)
  if ci.extra.healthbar then trackent.remove(ci, ci.extra.healthbar) end
  if invisibles[ci.clientnum] then ci.extra.healthbar = true return end
  ci.extra.healthbar = trackent.add(ci, function(i, lastpos)
    local o = vec3(lastpos.pos)
    o.z = o.z + 22
    ents.editent(i, server.PARTICLES, o, 5, 100, 0x3F0)
  end, false, true, nil)
end

spaghetti.addhook("damaged", function(info) updatehealthbar(info.target) end)

higherjump = function(ci)
  if ci.state.aitype ~= server.AI_NONE then return end
  if not jumpers[ci.clientnum] then jumpers[ci.clientnum] = true end
  playermsg("\f6Info\f7: \f0QUAD JUMP\f7: YOUR \f0JUMPING HEIGHT HAS QUADRUPLED!! \f7Use it to your advantage!", ci)
end

invisible = function(ci)
  local st = ci.state
  if st.aitype ~= server.AI_NONE then return end
  if server.m_ctf or server.m_hold then
    local flags, actorflags = server.ctfmode.flags, {} 
    for i = 0, flags:length() - 1 do if flags[i].owner == ci.clientnum then actorflags[i] = true end end
    if next(actorflags) then playermsg("\f6Info\f7: You \f3cannot \f7become invisible with the flag, but your egg has been counted!", ci) return end
  end
  if not invisibles[ci.clientnum] then 
    fireball(ci.state.o)
    for p in iterators.clients() do if p.clientnum ~= ci.clientnum then
      engine.sendpacket(p.clientnum, 0, putf({13, r = 1}, server.N_POS, {uint = ci.clientnum}, { ci.state.lifesequence % 2 * 8 }, emptypos):finalize(), -1) 
    end end

   if ci.extra.healthbar then 
     trackent.remove(ci, ci.extra.healthbar) 
     ci.extra.healthbar = true
   end

    ci.extra.bunnytail = trackent.add(ci, function(i, lastpos)
      local o = vec3(lastpos.pos)
      o.z = o.z + 5
      ents.editent(i, server.PARTICLES, o, 9, 1, 10, 0xFFF)
    end, false, false, nil)

  else spaghetti.cancel(invisibles[ci.clientnum]) end
  invisibles[ci.clientnum] = spaghetti.latergame(30000, function() 
    spaghetti.cancel(invisibles[ci.clientnum])
    invisibles[ci.clientnum] = nil
    disappear()
    if ci.extra.bunnytail then trackent.remove(ci, ci.extra.bunnytail) end
    fireball(ci.state.o)
    playermsg("\f6Info\f7: Your invisibility effect has expired.", ci)
    for p in iterators.clients() do if p.clientnum ~= ci.clientnum then
      playermsg("\f6Info\f7: " .. server.colorname(ci, nil) .. " is no longer invisible.", p)
    end end
    updatehealthbar(ci)
  end)
  disappear()
  playermsg("\f6Info\f7: \f0YOU ARE NOW \f3INVISIBLE!! \f7Use the \f0chainsaw \f7for sneak attacks, but caution, people can see your bunny tail!", ci)
  for p in iterators.clients() do if p.clientnum ~= ci.clientnum then
    playermsg("\f6Info\f7: " .. server.colorname(ci, nil) .. " is \f6INVISIBLE\f7! Watch your step (or their bunny tail)!", p)
  end end
end

extrafrags = function(ci)
  if ci.state.aitype ~= server.AI_NONE then return end
  local st, amount = ci.state, math.random(2, 7)
  if st.state ~= engine.CS_ALIVE then return end
  st.frags = st.frags + amount
  server.sendresume(ci)
  playermsg("\f6Info\f7: \f0QUAD LUCK\f7: YOU EFFORTLESSLY RECEIVED \f0" .. amount .. " EXTRA FRAGS!!", ci)
end

dizzy = function(ci)
  if ci.state.aitype ~= server.AI_NONE then return end 
  if ci.extra.dizzy then spaghetti.cancel(ci.extra.dizzy) end
  ci.extra.isdizzy = spaghetti.later(20000, function() 
    spaghetti.cancel(ci.extra.dizzy)
    ci.extra.isdizzy = nil 
    playermsg("\f6Info\f7: You have sobered up.", ci)
  end)
  ci.extra.dizzy = spaghetti.later(1000, function()
    if ci.extra.isdizzy then
      spaghetti.later(math.random(100, 900), function() 
        hitpush(ci, { x = math.random(-20, 20), y = math.random(-20, 20), z = 0 }) 
      end)
    end
  end, true)
  playermsg("\f6Info\f7: \f3TOO MANY SHOTS\f7: You are now drunk and dizzy, watch your step!", ci)
end


--[[

    CONFIG

    #########################################################################################################################
    #########################################################################################################################

    Use the easterents table below to map new easter ents to effects that are initialized above. Each easter ent should have the fields:
      
      -   n = "path/mapmodel" (eg. "dcp/jumppad2") [will be placed once on the egg's coordinates]
       OR   
          r = "server.ITEM" (eg. server.I_HEALTH, server.I_CARTRIDGES) [multiple instances will be displayed around the "egg"]

      -   effect = effectfunction() [initialized above]


    #########################################################################################################################

    Example 1:  Mix mapmodels and server.ITEMs for all effects (Eggs will look like their effects)

    local easterents = { 
      { n = "dcp/jumppad2", effect = higherjump },                             -- jump pad, jumping boost
      { r = server.I_HEALTH, effect = extrahealth },                           -- health boost, extra health
      { n = "vegetation/bush01", effect = invisible },                         -- bush, invisibility
      { r = server.I_CARTRIDGES, effect = extrafrags },                        -- pistol rounds, extra frags
      { n = "tentus/food-drink/winebottle", effect = dizzy, nocarrot = true }  -- wine bottle, random hitpush, don't also display a carrot
    }

    Example 2:  Only use the same mapmodel for all effects ("Surprise" eggs)

    local easterents = { 
      { n = "vegetation/bush01", effect = higherjump },
      { n = "vegetation/bush01", effect = extrahealth },
      { n = "vegetation/bush01", effect = invisible },
      { n = "vegetation/bush01", effect = extrafrags },
      { n = "vegetation/bush01", effect = dizzy }
    }
]]  

-- Current setup: Surprise eggs, but use carrot mapmodels as eggs

local easterents = { 
  { n = "carrot", effect = higherjump },
  { n = "carrot", effect = extrahealth },
  { n = "carrot", effect = invisible },
  { n = "carrot", effect = extrafrags },
  { n = "tentus/food-drink/winebottle", effect = dizzy, nocarrot = true }
}
   
local maxeggs = 2        -- maximum eggs that are available at once

local egginterval = 15   -- egg spawn interval in seconds

local showsparks = true  -- show some pretty sparks around eggs

--  #########################################################################################################################
--  #########################################################################################################################


-- add coordinates from file, line format should be 'mapname pos-x pos-y pos-z'
spaghetti.addhook("entsloaded", function(info)
  if not enable then return end
  for k, o in pairs(easter) do easter[k] = nil end
  local k = 0
  local f = io.open(eggfile)
  if not f then return end
  for line in f:lines() do
    local map, x, y, z = line:match("([^%s]+)%s(%d*%.?%d+)%s(%d*%.?%d+)%s(%d*%.?%d+)")
    if map and map == server.smapname and x and y and z then 
      k = k + 1
      easter[k] = vec3({ x = x, y = y, z = z })
    end
  end
  f:close()
end)

-- item spawning and pickup
function t(o)
  local count = 0
  for _ in pairs(o) do count = count + 1 end
  return count
end

local function spawnrandoments(max)
  if not enable or not server.m_noitems or not ents.active() or not next(easter) or (max and (max > maxeggs)) or (t(currentents) >= maxeggs) or easteredit then return end
  local occupied, n, num, max = {}, -1, 0, max and max or maxeggs
  local eastercount = {}
  for n, _ in pairs(easter) do table.insert(eastercount, n) end
  while (num < max) and next(eastercount) ~= nil do
    num = num + 1
    local randent = easterents[math.random(#easterents)]
    repeat
      n = table.remove(eastercount, math.random(#eastercount))
    until num > #easter or not next(eastercount) or (not occupied[n] and not currentents[n])
    if occupied[n] or currentents[n] then return end
    occupied[n] = true
    local d = easter[n]
    if not d then return end
    
    local x, y, z = d.x, d.y, d.z
    local etype = randent.n and (not hasmmreset() and server.MAPMODEL or server.CARROT) or randent.r
    local r = not not randent.r
    local name = randent.n and (not hasmmreset() and randent.n or 0) or 0
    local carrot = (not r and (randent.n ~= "carrot") and not randent.nocarrot) and ents.newent(server.CARROT, { x = x, y = y, z = z }) or nil -- always spawn a server.CARROT

    currentents[n] = { 
      o = easter[n], 
      effect = randent.effect or nil,
      r = r,
      carrot = carrot
    }
    
    if r then -- spawn them around the egg
      currentents[n].i = ents.newent(server.CARROT, { x = x, y = y, z = z })
      currentents[n].i1 = ents.newent(etype, { x = x + 4, y = y, z = z }, 0, name)
      currentents[n].i2 = ents.newent(etype, { x = x - 4, y = y, z = z }, 0, name)
      currentents[n].i3 = ents.newent(etype, { x = x, y = y + 4, z = z }, 0, name)
      currentents[n].i4 = ents.newent(etype, { x = x, y = y - 4, z = z }, 0, name)      
    else currentents[n].i = ents.newent(etype, { x = x, y = y, z = z }, 0, name) end -- spawn below egg

    if carrot then ents.setspawn(carrot, true) end
    ents.setspawn(currentents[n].i, true)
    if r then 
      ents.setspawn(currentents[n].i1, true) ents.setspawn(currentents[n].i2, true)
      ents.setspawn(currentents[n].i3, true) ents.setspawn(currentents[n].i4, true)  
    end
    if showsparks then sparkle(currentents[n]) end
  end
end

spaghetti.addhook("connected", function(info) 
  for n, egg in pairs(currentents) do
    if egg.carrot then ents.setspawn(egg.carrot, true, true) end
    ents.setspawn(egg.i, true, true)
    if egg.r then
      ents.setspawn(egg.i1, true, true) ents.setspawn(egg.i2, true, true)
      ents.setspawn(egg.i3, true, true) ents.setspawn(egg.i4, true, true)  
    end
  end
end)

local function eggpickup(ci, n, editing)
  if not ci and not editing then return end
  local egg = currentents[n]
  if not egg then return end
  if egg.carrot then ents.setspawn(egg.carrot, false) ents.delent(egg.carrot) end
  ents.setspawn(egg.i, false) ents.delent(egg.i)
  if egg.r then  
    ents.setspawn(egg.i1, false) ents.delent(egg.i1) 
    ents.setspawn(egg.i2, false) ents.delent(egg.i2) 
    ents.setspawn(egg.i3, false) ents.delent(egg.i3) 
    ents.setspawn(egg.i4, false) ents.delent(egg.i4) 
  end
  if egg.sparkle then spaghetti.cancel(egg.sparkle) end
  if egg.signal then ents.delent(egg.signal) end
  currentents[n] = nil
  if editing then return end
  spaghetti.latergame(egginterval * 1000, function()
    spawnrandoments(1)
  end)
  if ci.state.aitype ~= server.AI_NONE then return end
  playermsg("\f7=============\n\f6Info\f7: \f0YOU FOUND AN EASTER EGG!!", ci)
  sound(ci, server.S_ITEMAMMO) sound(ci, server.S_ITEMAMMO)
  ci.extra.eggs = ci.extra.eggs + 1
  if enable and egg.effect then egg.effect(ci) end
end

spaghetti.addhook("positionupdate", function(info) 
  if not next(easter) or not next(currentents) or (info.cp.state.aitype ~= server.AI_NONE) or easteredit then return end
  local last = vec3(info.lastpos.pos)
  for n, egg in pairs(currentents) do 
    if last:dist(egg.o) <= 20 then
      eggpickup(info.cp, n)
      return
    end
  end
end)

spaghetti.addhook("changemap", function() spawnrandoments() end)


-- various event hooks

-- jump hook
spaghetti.addhook(server.N_SOUND, function(info) 
  if info.skip or info.cq.state.aitype ~= server.AI_NONE then return end
  if info.sound == server.S_JUMP and jumpers[info.cq.clientnum] then 
    hitpush(info.cq, { x = 0, y = 0, z = 250 }) 
  end 
end)

-- effect cancellation
local function clearclienteffects(ci)
  if ci.state.aitype ~= server.AI_NONE then return end
  local hadeffect = false
  if jumpers[ci.clientnum] then hadeffect, jumpers[ci.clientnum] = true, nil end
  if invisibles[ci.clientnum] then 
    spaghetti.cancel(invisibles[ci.clientnum])
    hadeffect, invisibles[ci.clientnum] = true, nil 
  end
  if ci.extra.bunnytail then trackent.remove(ci, ci.extra.bunnytail) end
  if ci.extra.healthbar then 
    hadeffect = true
    trackent.remove(ci, ci.extra.healthbar)
    server.sendresume(ci)
  end
  if ci.extra.isdizzy then 
    spaghetti.cancel(ci.extra.isdizzy)
    spaghetti.cancel(ci.extra.dizzy)
    hadeffect = true
  end
  return hadeffect
end

spaghetti.addhook("clientdisconnect", function(info) clearclienteffects(info.ci) end)

spaghetti.addhook("notalive", function(info)
  if clearclienteffects(info.ci) then
    info.ci.extra.hadeffect = true
    playermsg("\f6Info\f7: Your effect has expired.", info.ci)
  end
end)

spaghetti.addhook("spawned", function(info)  -- reset health
  if info.ci.extra.hadeffect then
    info.ci.state.maxhealth, info.ci.extra.hadeffect = 100, nil
    server.sendresume(info.ci)
  end
end)

spaghetti.addhook("prechangemap", function(info) 
  for k, v in pairs(easter) do easter[k] = nil end
  for n, egg in pairs(currentents) do 
    if egg.sparkle then spaghetti.cancel(egg.sparkle) end
    currentents[n] = nil 
  end
  for p in iterators.clients() do if clearclienteffects(p) then playermsg("\f6Info\f7: Your effect has expired.", p) end end
  easteredit = false
end)


-- count collected eggs and persist records
spaghetti.addhook("connected", function(info) info.ci.extra.eggs = 0 end)
spaghetti.addhook("changemap", function(info) for p in iterators.clients() do p.extra.eggs = 0 end end)

spaghetti.addhook("intermission", function(info) 
  if not enable then return end
  local highscore = { eggs = 0, cn = -1 }
  for ci in iterators.select(L"_.extra.eggs > 0") do 
    if ci.extra.eggs > highscore.eggs then highscore.eggs, highscore.cn = ci.extra.eggs, ci.clientnum end
  end
  if highscore.cn == -1 then return end
  local ci = server.getinfo(highscore.cn)
  if not ci then return end
  spaghetti.later(50, function(info) -- should be last intermission message
    server.sendservmsg("\f6Info\f7: \f0" .. server.colorname(ci, nil) .. " \f7has collected the most eggs! Total count: \f0" .. highscore.eggs)
    local file = jsonpersist.load(servertag.fntag .. "easterrecords") or {}
    local record = file[server.smapname] or {}
    local maprecord, maprecordholder = record.eggs, record.name
    if not maprecord or highscore.eggs > maprecord then
      server.sendservmsg("\f6Info\f7: That is a \f3NEW MAP RECORD!!")
      file[server.smapname] = { eggs = highscore.eggs, name = ci.name }
      jsonpersist.save(file, servertag.fntag .. "easterrecords")
    else server.sendservmsg("\f6Info\f7: Map record: " .. maprecord .. " eggs by " .. maprecordholder .. ".") end
  end)
end)


-- invisibility hooks & logic, originally written by pisto
-- taken from https://github.com/pisto/spaghettimod-assorted/blob/master/load.d/1000-honzik-server.lua
disappear = function()
  if not enable then return end
  local players = map.sf(L"_.state.state == engine.CS_ALIVE and _ or nil", iterators.clients())
  for viewer in pairs(players) do for vanish in pairs(players) do 
    if vanish.clientnum ~= viewer.clientnum and invisibles[vanish.clientnum] then
      local p = putf({ 30, r = 1}, server.N_SPAWN)
      server.sendstate(vanish.state, p)
      engine.sendpacket(viewer.clientnum, 1, n_client(p, vanish):finalize(), -1)
    end 
  end end
end

spaghetti.later(900, disappear, true)

spaghetti.addhook("connected", function(info)
  if info.ci.state.state == engine.CS_SPECTATOR then spectators[info.ci.clientnum] = true return end
end)

spaghetti.addhook("specstate", function(info)
  if not enable then return end
  if info.ci.state.state == engine.CS_SPECTATOR then spectators[info.ci.clientnum] = true return end
  spectators[info.ci.clientnum] = nil
  --clear the virtual position of players so sounds do not get played at random locations
  local p
  for ci in iterators.clients() do if ci.clientnum ~= info.ci.clientnum and invisibles[info.ci.clientnum] then
    p = putf(p or {13, r = 1}, server.N_POS, {uint = ci.clientnum}, { ci.state.lifesequence % 2 * 8 }, emptypos)
  end end
  if not p then return end
  engine.sendpacket(info.ci.clientnum, 0, p:finalize(), -1)
end)

spaghetti.addhook("clientdisconnect", function(info) spectators[info.ci.clientnum] = nil end)

spaghetti.addhook("worldstate_pos", function(info)
  if not enable or not invisibles[info.ci.clientnum] then return end
  info.skip = true
  local position = info.ci.position.buf
  local p = engine.enet_packet_create(position, 0)
  for scn in pairs(spectators) do engine.sendpacket(scn, 0, p, -1) end
  server.recordpacket(0, position)
end)

-- nerf invisibility item: no flag pickup, only chainsaw damage
spaghetti.addhook(server.N_TAKEFLAG, function(info)
  if info.skip or not invisibles[info.ci.clientnum] or (info.ci.state.aitype ~= server.AI_NONE) then return end
  info.skip = true
  playermsg("\f6Info\f7: You \f3cannot \f7interact with the flag while invisible!", info.ci)
end)

-- no shot fx
spaghetti.addhook(server.N_SHOOT, function(info)
  if (info.shot.gun == server.GUN_FIST) or (info.ci.state.aitype ~= server.AI_NONE) or not invisibles[info.ci.clientnum] then return end
  --info.skip = true  -- skipping info could give message error, rather put gun = 0
  info.shot.gun = 0
  info.shot.from[0], info.shot.to[0] = -1e7, -1e7
  info.shot.from[1], info.shot.to[1] = -1e7, -1e7
  info.shot.from[2], info.shot.to[2] = -1e7, -1e7
  playermsg("\f6Info\f7: You can only attack with \f3CHAINSAW \f7while invisible!", info.ci)
end)

spaghetti.addhook("dodamage", function(info)
  if info.skip or not invisibles[info.actor.clientnum] or info.actor.clientnum == info.target.clientnum or (info.actor.state.aitype ~= server.AI_NONE) then return end
  if (info.gun ~= server.GUN_FIST) then -- rifle hits will still be possible in chainsaw range since every shot is now server.GUN_FIST
    info.skip = true
  else
     playermsg("\f6Info\f7: You fell into the chainsaw of the \f6INVISIBLE \f7" .. server.colorname(info.actor, nil) .. "! Better keep moving!", info.target) 
  end
end)

spaghetti.addhook("damageeffects", function(info)
  if info.skip or not invisibles[info.actor.clientnum] or info.actor.clientnum == info.target.clientnum or (info.actor.state.aitype ~= server.AI_NONE) then return end
  if (info.gun ~= server.GUN_FIST) then 
    info.skip = true 
    local push = info.hitpush
    push.x, push.y, push.z = 0, 0, 0
  end
end)


-- add a toggle
commands.add("easter", function(info) 
  if info.ci.privilege < server.PRIV_AUTH then return playermsg("Access denied.", info.ci) end
  for ci in iterators.all() do clearclienteffects(ci) end
  disappear()
  enable = not enable
  playermsg("\f6Info\f7: Easter mode is " .. (enable and "enabled" or "disabled") .. " starting next map change.", info.ci)
end)

spaghetti.addhook("changemap", function(info) 
  if enable then 
    spaghetti.latergame(50000, function() 
      if (server.interm > 0) then return end
      server.sendservmsg("\f6Info\f7: \f0EASTER MODE IS ACTIVE! \n\f6Info\f7: Collect easter eggs to beat the current record, and to gain special abilities: \f6HIGHER \f7jumping, \f6INVISIBILITY \f7and more!")
    end, true)
  end
end)

spaghetti.addhook(server.N_ADDBOT, function(info)
  if info.skip then return end
  if enable then
    info.skip = true
    playermsg("\f6Info\f7: Bots cannot be added in easter mode.", info.ci)
    return
  end
end)

-- add egg edit mode
local function simulateegg(o)
  local n = 1
  for _ in pairs(currentents) do n = n + 1 end
  currentents[n] = { o = o }
  currentents[n].i = ents.newent(server.CARROT, { x = o.x, y = o.y, z = o.z }, 0, 0)
  currentents[n].signal = ents.newent(server.PARTICLES, { x = o.x, y = o.y, z = o.z }, 7, 0, 600, 0xFFF )
  ents.setspawn(currentents[n].i, true)
end

local function refresheggs()
  for n, egg in pairs(currentents) do eggpickup(nil, n, true) end
  for k, o in pairs(easter) do easter[k] = nil end
  local elhook = spaghetti.hooks.entsloaded
  if elhook then elhook({}) end
  for k, o in pairs(easter) do simulateegg(o) end
end

function traverseeggs(filename, remove_line)
  local fp = io.open(filename, "r")
  if fp == nil then return nil end
  content = {}
  i = 1;
  for line in fp:lines() do
    if (remove_line and (i < remove_line or i > remove_line)) or (not remove_line and (line ~= "")) then
      content[#content+1] = line
    end
    i = i + 1
  end
  fp:close()
  fp = io.open(filename, "w+")
  for i = 1, #content do fp:write(string.format("%s\n", content[i])) end
  fp:close()
end

commands.add("addegg", function(info)
  if info.ci.privilege < server.PRIV_MASTER then playermsg("Access denied.", info.ci) return end
  if not easteredit then return playermsg("\f6Info\f7: Please enable egg edit mode with \f0#editeggs\f7 in order to add eggs.", info.ci) end         
  local file = io.open(eggfile, "a")
  local o = vec3(info.ci.state.o)
  file:write("\n" .. server.smapname .. " " .. o.x .. " " .. o.y .. " " .. o.z)
  file:close()
  traverseeggs(eggfile)
  refresheggs()
  server.sendservmsg("\f6Info\f7: " .. info.ci.name .. " successfully added egg at \f0" .. tostring(o) .. "\f7!", info.ci)
end, "#addegg: Add an easter egg where you're standing. Only works in egg edit mode! -> #editeggs")

commands.add("removeegg", function(info)
  if info.ci.privilege < server.PRIV_MASTER then playermsg("Access denied.", info.ci) return end
  if not easteredit then return playermsg("\f6Info\f7: Please enable egg edit mode with \f0#editeggs\f7 in order to add eggs.", info.ci) end         
  local file = io.open(eggfile)
  local o, i, found = vec3(info.ci.state.o), 0, false
  for line in file:lines() do
    i = i + 1
    local map, x, y, z = line:match("([^%s]+)%s(%d*%.?%d+)%s(%d*%.?%d+)%s(%d*%.?%d+)")
    if map and map == server.smapname and x and y and z then
      local eggpos = vec3({ x = x, y = y, z = z })
      if eggpos:dist(o) < 20 then 
        for n, egg in pairs(currentents) do
          if egg.o.x == eggpos.x and egg.o.y == eggpos.y and egg.o.z == eggpos.z then
            eggpickup(nil, n, true)
            easter[n], found = nil, true
            server.sendservmsg("\f6Info\f7: " .. info.ci.name .. " successfully \f3deleted \f7egg at \f0" .. tostring(eggpos) .. "!")
            traverseeggs(eggfile, i)
            refresheggs()
          end
        end
      end
    end
  end
  file:close()
  if not found then playermsg("\f6Info\f7: No eggs found at your position! \f6Please stand right on top of the egg in order to remove it!", info.ci) end
end, "#removeegg: Remove the easter egg that you're standing on. Only works in egg edit mode! -> #editeggs")

commands.add("editeggs", function(info)
  if info.ci.privilege < server.PRIV_AUTH then playermsg("Access denied.", info.ci) return end
  if not easter then return playermsg("\f6Info\f7: Easter mode not enabled.", info.ci) end
  easteredit = not easteredit
  if easteredit then
    for i, egg in pairs(currentents) do eggpickup(nil, i, true) end
    refresheggs()
  else
    for i, egg in pairs(currentents) do eggpickup(nil, i, true) end
    spawnrandoments()
  end
  server.sendservmsg("\f6Info\f7: Egg edit mode ".. (easteredit and "\f0en" or "\f3dis") .. "abled\f7!")
  if easteredit then server.sendservmsg("\f6Info\f7: Add and remove eggs with \f0#addegg \f7and \f0#removeegg \f7at the position you're standing at!") end
end, "#editeggs: Toggle egg edit mode to display all egg locations, and add or delete eggs.")
