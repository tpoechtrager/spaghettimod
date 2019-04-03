--[[
    Communicate with the NodeJS discord bot through UDP. 
    The NodeJS bot can serve multiple gameservers which are identified by their unique serverports.
    This module features hooks to broadcast server messages and stats, as well as a handler for incoming discord commands.
]]

local iterators, commands, playermsg, ban = require"std.iterators", require"std.commands", require"std.playermsg", require"std.ban"
local putf, n_client, ipextra = require"std.putf", require"std.n_client", require"std.ipextra"
local fp, L, ip, jsonpersist = require"utils.fp", require"utils.lambda", require"utils.ip", require"utils.jsonpersist"
local map, pick, I = fp.map, fp.pick, fp.I

local js, socket, udp = require"dkjson", require"socket", nil

local module = { 
  config = { 
    udp = nil, 
    receiver = nil,
    connected = connected, 
    tag = cs.serverport, 
    host = "", 
    port = "", 
    channel = "", 
    scoreboardChannel = "", 
    commands = {} 
  } 
}

-- some utils
local unixtime = os.time
local unixprint = L"_ and os.date('!%c', _ and math.modf(_) or nil) .. ' UTC' or 'permanent'"

-- escape discord special chars
local rep = { ["%_"] = "\\_", ["%*"] = "\\*", ["`"] = "\\`", ["~"] = "\\~", ["|"] = "\\|" }
local function c(s)
  if not s then return nil end
  s = string.gsub(s, "\\", "")
  for k, v in pairs(rep) do s = string.gsub(s, k, v) end
  return s
end

-- discord doesn't accept more than 2048 characters per msg
local function trim(str, custommax)
  if not str then return nil end
  local len, max = string.len(str), custommax or 1950
  if len > max then str = string.sub(str, 1, max) .. "\n..." end
  return str, len > max
end

local function ciip(cn)
  return engine.ENET_NET_TO_HOST_32(engine.getclientip(cn))
end


--[[
    UDP
]]

local function formatUDP(tbl) return js.encode(jsonpersist.deepencodeutf8(tbl), { indent = false }) end

local function sendUDP(msg)
  local tbl = {}
  tbl.tag = cs.serverport
  tbl.channel = module.config.channel
  tbl.scoreboardChannel = module.config.scoreboardChannel
  tbl.msg = msg
  if module.config.connected then
    local res = udp:sendto(formatUDP(tbl), socket.dns.toip(module.config.host), module.config.port)
    if not res then engine.writelog("[Discord] Could not send message to discord interface.") end
  end
end

local function createUDP(host, port, tag, channel, scoreboardChannel)
  udp = socket.udp()
  if not udp then
    engine.writelog("[Discord] Could not create new UDP.")
    return false
  end
  udp:settimeout(0)
  engine.writelog("[Discord] Trying to connect to node discord interface...")
  udp:sendto(formatUDP({ tag = tag, channel = channel, scoreboardChannel = scoreboardChannel, msg = { event = "register", server = cs.serverdesc } }), socket.dns.toip(host), port)
  if module.config.receiver then spaghetti.cancel(module.config.receiver) end
  module.config.receiver = spaghetti.later(50, function()
    local datagram = udp:receive()
    if datagram then
      local info, pos, err = js.decode(jsonpersist.deepdecodeutf8(datagram), 1, nil)
      if info and not err then 
        local cmd, args = info.cmd, info.args
        if not info.user and (cmd == "#regsuccess") then 
          engine.writelog("[Discord] Connected to discord interface!")
          module.config.connected = true
          return
        end
        if not info.user then return end
        engine.writelog("[Discord] Command: " .. info.user .. ": " .. info.prefix .. info.cmd .. " " .. (info.args and info.args or "")) 
        if not module.config.commands[cmd] then    
          local msg = "Command **" .. cmd .. "** not found.\nType " .. info.prefix .. "help for a list of available commands."
          local res = { event = "cmderr", user = info.user, userid = info.userid, msg = msg } 
          sendUDP(res)
        else
          module.config.commands[cmd].fn(info)
        end
      end
    end
  end, true)
  spaghetti.later(1000, function() 
    if not module.config.connected then
      if module.config.receiver then spaghetti.cancel(module.config.receiver) end
      engine.writelog("[Discord] Could not connect to discord interface.")
      engine.writelog("[Discord] Please configure and launch the NodeJS app.")
    else
      module.config.udp = udp
    end
  end)
  return true
end

local function new(info)
  if not info.relayHost or not info.relayPort or not info.discordChannelID or info.discordChannelID == "my-discord-channel-id" then 
    engine.writelog("[Discord] Incomplete configuration. Check new().")
    return
  end
  if info.scoreboardChannelID and info.discordChannelID == info.scoreboardChannelID then 
    engine.writelog("[Discord] Error: Main channel and scoreboard channel must not be the same, not starting.")
    return
  end
  module.config.host, module.config.port = info.relayHost, info.relayPort
  module.config.channel, module.config.scoreboardChannel = tostring(info.discordChannelID), info.scoreboardChannelID and tostring(info.scoreboardChannelID) or nil
  return createUDP(info.relayHost, info.relayPort, cs.serverport, info.discordChannelID, info.scoreboardChannelID)
end

commands.add("restartdiscord", function(info) 
  if info.ci.privilege < server.PRIV_ADMIN then return playermsg("Access denied.", info.ci) end  
  if not module.config.host or not module.config.port or not module.config.channel then
    return playermsg("\f6Info\f7: No configuration found. Please provide the main config and restart the server.", info.ci)
  end
  createUDP(module.config.host, module.config.port, cs.serverport, module.config.channel)
  playermsg("\f6Info\f7: A new connection to the Discord interface has been launched.", info.ci)
end)


--[[
    Broadcast server events
]]

-- useful functions
local function prettyname(ci)
  if not ci then return "" end
  return ci.name .. " (" .. ci.clientnum .. ")"
end

local function prettygeoip(geoip, short)
  local format, country, region, city = {}, geoip.country and ((geoip.country.names and geoip.country.names.en) or geoip.country.en), geoip.subdivisions and geoip.subdivisions[1].names.en, geoip.city and geoip.city.names.en
  if short then return country and engine.decodeutf8(country) or "Unknown" end
  if city then table.insert(format, engine.decodeutf8(city)) end
  if region and not (city and region:find(city, 1, true)) then table.insert(format, engine.decodeutf8(region)) end
  if country then table.insert(format, engine.decodeutf8(country)) end
  return table.concat(format, ", ")
end

local function prettytime(millis)
  local tbl = os.date("!*t", (unixtime() - millis))
  local msg = ""
  if tbl.hour ~= 0 then msg = msg .. tbl.hour .. (" hour%s "):format(tbl.hour == 1 and "" or "s") end
  if tbl.min ~= 0 then msg = msg .. tbl.min .. (" minute%s "):format(tbl.min == 1 and "" or "s") end
  if tbl.sec ~= 0 then msg = msg .. tbl.sec .. (" second%s"):format(tbl.sec == 1 and "" or "s")  end
  return msg
end

-- mark high frags/acc/tk in the scoreboard code block 
local high = {
  ["10"] = 90, 
  ["15"] = 80,  
  ["25"] = 70, 
  ["40"] = 65
}  

local function prefix(ci, n)
  if not n then return "" end
  local frags, acc, tk = ci.state.frags, ci.state.damage*100/math.max(ci.state.shotdamage, 1), ci.state.teamkills
  for f, a in pairs(high) do
    if frags >= tonumber(f) and tonumber(acc) >= a then return "->ACC: " end
  end
  if tk >= 3 then return "->TK:  " end
  return ("#" .. n .. string.rep(" ", (5 - #tostring(n))) .. " ")
end

local function playerStats(ci, n, includename, includeflags)
  if not ci then return "" end
  local name, cn = ci.name, tostring(ci.clientnum)
  local spaces = n and string.rep(" ", (18 - (#name + #cn))) or ""
  local namestr = includename and prefix(ci, n) .. name .. " (" .. cn .. "): " .. spaces or ""
  local flags = includeflags and (server.m_ctf or server.m_hold) and "Fl: " .. ci.state.flags .. ", " or ""
  local frags, deaths = tostring(ci.state.frags), tostring(ci.state.deaths) 
  local kd = ("%.1f"):format(ci.state.frags/math.max(ci.state.deaths, 1))
  local acc = ("%.0f%%"):format(ci.state.damage*100/math.max(ci.state.shotdamage, 1))
  local tk = tostring(ci.state.teamkills)
  local fmt = "%s%sK/D %s/%s (%s), %s, TK %s"
  return (fmt):format(namestr, flags, frags, deaths, kd, acc, tk)
end

local function listPlayers(team) 
  local str, d, n = "", 0, 0
  local points = {}
  if team then
    for ci in iterators.inteam(team) do
      points[ci.clientnum] = ci.state.frags + ci.state.flags * 100 
      d = d + 1
    end
  else
    for ci in iterators.players() do
      points[ci.clientnum] = ci.state.frags + ci.state.flags * 100 
      d = d + 1
    end
  end
  while n <= d do
    n = n + 1
    local bestcn, bestscore = 0, -1000
    for cn, score in pairs(points) do if score > bestscore then bestcn, bestscore = cn, score end end
    if bestcn == nil then break end
    local ci = server.getinfo(bestcn)
    str = str .. playerStats(ci, n, true, server.m_ctf or server.m_hold) .. "\n"
    points[bestcn] = nil
    if not next(points) then break end
  end
  for k,v in pairs(points) do points[k] = nil end
  if (str == "") or (str == "\n") then return nil else return str end
end

local function gameStatus(freshconnect)
  local str = ""
  local modemap = ("%s on %s"):format(server.modename(server.gamemode, "unknown"), c(server.smapname))   
  local players = ("%s/%s players"):format(freshconnect and 1 or server.numclients(-1, false, true), cs.maxclients)
  if server.m_teammode then
    local scores = {} 
    for ci in iterators.players() do 
      scores[ci.team] = server.smode and server.smode:getteamscore(ci.team) or (ci.team ~= "") and server.addteaminfo(ci.team).score
      if not scores[ci.team] then
        scores[ci.team] = 0
        for p in iterators.inteam(ci.team) do scores[ci.team] = scores[ci.team] + p.state.frags end
      end
    end
    local temp, fin = {}, {}
    for k, v in pairs(scores) do table.insert(temp, v) end
    table.sort(temp, function(a, b) return a > b end)
    for i, s in ipairs(temp) do 
      for k, v in pairs(scores) do if (s == v) and not fin[k] then 
        fin[k] = v
        str = str .. "Team " .. k .. " (" .. scores[k] .. "): \n\n" .. listPlayers(k) .. "\n" 
      end end      
    end
  else
    str = str .. listPlayers(nil)
  end
  return modemap, players, (str == "") and "[Nobody playing]" or str
end

-- hooks
spaghetti.addhook("enterlimbo", function(info)
  info.ci.extra.curpriv = info.ci.privilege
end)

spaghetti.addhook("connected", function(info)
  info.ci.extra.curpriv = info.ci.privilege
  info.ci.extra.contime = unixtime()
  local loc = info.ci.extra.geoip and prettygeoip(info.ci.extra.geoip, true) or "Unknown"
  local res = { event = "connected", name = c(info.ci.name), clientnum = info.ci.clientnum, country = c(loc) } 
  sendUDP(res)
end)

spaghetti.addhook("clientdisconnect", function(info)
  if info.ci.extra.limbo then return end -- prevent limbo timeout disconnect messages
  local contime = prettytime(info.ci.extra.contime)
  local reason, iskick = engine.disconnectreason(info.reason), info.ci.extra.iskicked or (info.reason == engine.DISC_KICK)
  local reasonstr = (reason and " [" .. reason .. "]") or (iskick and " [kicked/banned]") or ""
  local res = { event = "disconnected", name = c(info.ci.name), clientnum = info.ci.clientnum, contime = contime, iskick = iskick, reason = reasonstr } 
  sendUDP(res)
end, true)

spaghetti.addhook(server.N_TEXT, function(info)
  if info.skip or info.ci.extra.ipextra.muted then return end
  if server.interm > 0 and (tonumber(info.text) == 1 or tonumber(info.text) == 2) then return end  -- reduce mapbattle spam
  local res = { event = "chat", name = c(info.ci.name), clientnum = info.ci.clientnum, txt = c(engine.filtertext(info.text, true, true)) }
  sendUDP(res)
end)

spaghetti.addhook(server.N_SWITCHNAME, function(info)
  if info.skip or info.ci.extra.ipextra.muted or (info.text == info.ci.name) then return end
  local oldname = c(engine.filtertext(info.ci.name):sub(1, server.MAXNAMELEN):gsub("^$", "unnamed"))
  local newname = c(engine.filtertext(info.text):sub(1, server.MAXNAMELEN):gsub("^$", "unnamed"))
  local res = { event = "rename", oldname = c(oldname), newname = c(newname), clientnum = info.ci.clientnum }
  sendUDP(res)
end)

spaghetti.addhook("changemap", function(info)
  local hasplayers = server.numclients(-1, false, true) > 0
  local modemap, players, str = gameStatus(not hasplayers)
  local res = { event = "newgame", map = c(server.smapname), modemap = modemap, map = c(server.smapname), nmap = server.smapname, players = players, str = trim(str) }
  sendUDP(res)
  local n = 0
  spaghetti.latergame(1000 * 60, function()
    n = n + 1
    if (n > 9) or (server.interm > 0) then return end
    local modemap, players, str = gameStatus()
    local res = { event = "status", n = tonumber(n), progress = "(" .. tonumber(n) .. "/10)", modemap = modemap, map = c(server.smapname), nmap = server.smapname, players = players, str = trim(str) }
    sendUDP(res)
  end, true)
end)

spaghetti.addhook("intermission", function()    
  local modemap, players, str = gameStatus()
  local res = { event = "intermission", modemap = modemap, map = c(server.smapname), nmap = server.smapname, players = players, str = trim(str) }
  sendUDP(res) 
end)

spaghetti.addhook("noclients", function(info)
  local modemap, players, str = gameStatus()
  local res = { event = "status", progress = "(EMPTY)", modemap = modemap, map = c(server.smapname), nmap = server.smapname, players = players, str = "[No players connected]" }
  sendUDP(res)
end, true)

spaghetti.addhook("master", function(info)
  local action = ""
  if info.authname then 
    action = ("%s as '%s'%s"):format((info.privilege ~= server.PRIV_NONE and info.privilege > info.ci.extra.curpriv) and "claimed " .. server.privname(info.privilege) or "authenticated", c(info.authname), (info.authdesc and info.authdesc ~= "") and " [".. c(info.authdesc) .."]" or "")
    info.ci.extra.curpriv = info.privilege > info.ci.extra.curpriv and info.privilege or info.ci.extra.curpriv
  else 
    action = (info.privilege > server.PRIV_NONE or info.authname) and "claimed " .. server.privname(info.privilege) or "relinquished " .. server.privname(info.ci.extra.curpriv) or ""
    info.ci.extra.curpriv = info.privilege
  end
  local res = { event = "master", name = c(info.ci.name), clientnum = info.ci.clientnum, action = action }
  sendUDP(res) 
end)

local lastkicks = {}
spaghetti.addhook("kick", function(info)
  info.c.extra.iskicked = true
  local actorstr = info.actor and prettyname(info.actor) or "The server"
  local kickip = tostring(ip.ip(info.c))
  if lastkicks[kickip] then return end
  lastkicks[kickip] = spaghetti.later(1000, function() lastkicks[kickip] = nil end)  -- only show an addban once
  local str = ("%s bans %s"):format(c(actorstr), kickip)
  local res = { event = "kick", str = str }
  sendUDP(res) 
end)


--[[
    Add some discord commands.
    Format: addcommand({cmdname[, alias1, alias2 ...]}, callback({user, userid, prefix, cmd, args}), argsstring, cmdhelpstring)
]]

local cmdaliases = {}
local function addcommand(cmd, fn, cmdargs, help)
  if not cmd or not fn then return end
  if not cmdargs then cmdargs = "" end
  if not help then help = "" end
  if type(cmd) == "table" then
    local alias = {}
    for _, c in ipairs(cmd) do
      module.config.commands[c] = { fn = fn, cmdargs = cmdargs, help = help }
      table.insert(alias, tostring(c))
    end
    for _, c in ipairs(cmd) do cmdaliases[c] = alias end  
    return
  else
    module.config.commands[cmd] = { fn = fn, cmdargs = cmdargs, help = help }
    return
  end
end

-- simple helper
local function cmderror(info, msg)
  local res = { event = "cmderr", user = info.user, userid = info.userid, msg = msg }
  sendUDP(res)
end

-- re-implement some commands
addcommand("say", function(info) 
  if not info.args or info.args == "" then return cmderror(info, "You need to enter a string.") end
  local text = engine.decodeutf8(info.args)
  local name = engine.filtertext(info.user):sub(1, server.MAXNAMELEN):gsub("^$", "unnamed")
  local fakecn = 0
  while engine.getclientinfo(fakecn) ~= nil and fakecn < 127 do fakecn = fakecn + 1 end
  engine.sendpacket(-1, 1, putf({#name, r = 1}, server.N_INITCLIENT, fakecn, name, -1, 0):finalize(), -1)
  engine.sendpacket(-1, 1, n_client(putf({#text + 9, r = 1}, server.N_TEXT, "[REMOTE] " .. text), fakecn):finalize(), -1)
  engine.sendpacket(-1, 1, putf({1, r = 1}, server.N_CDIS, fakecn):finalize(), -1)
  local res = { event = "cmdsay", name = c(name), txt = c(engine.filtertext(text, true, true)) }
  sendUDP(res)
end, "<msg>", "Send a message to the server.")

addcommand({"status", "players"}, function(info) 
  if server.numclients(-1, false, true) == 0 then return cmderror(info, "No players connected.") end
  local modemap, players, str = gameStatus()
  local res = { event = "cmdstatus", modemap = modemap, map = c(server.smapname), nmap = server.smapname, players = players, str = trim(str) }
  sendUDP(res)
end, nil, "View the server status.")

addcommand("stats", function(info) 
  if not info.args or info.args == "" then return cmderror(info, "You need to enter a cn.") end
  local cn = tonumber(info.args)
  if not cn or not engine.getclientinfo(cn) then return cmderror(info, "Player not found.") end
  local ci = engine.getclientinfo(cn)
  local location = ci.extra.geoip and prettygeoip(ci.extra.geoip) or "Unknown"
  local country = ci.extra.geoip and prettygeoip(ci.extra.geoip, true) or "Unknown"
  local contime = prettytime(ci.extra.contime)
  local stats = playerStats(ci, nil, false, server.m_ctf or server.m_hold)  
  local res = { event = "cmdstats", name = ci.name, cn = ci.clientnum, location = c(location), country = c(country), contime = contime, stats = stats }
  sendUDP(res) 
end, "<cn>", "See the stats of player <cn>.")

addcommand("getip", function(info)
  if not info.args or info.args == "" then return cmderror(info, "You need to enter a cn.") end
  local cn = tonumber(info.args)
  if not cn or not engine.getclientinfo(cn) then return cmderror(info, "Player not found.") end
  local ci = engine.getclientinfo(cn)
  local location = ci.extra.geoip and prettygeoip(ci.extra.geoip) or "Unknown"
  local country = ci.extra.geoip and prettygeoip(ci.extra.geoip, true) or "Unknown"
  local contime = prettytime(ci.extra.contime)
  local res = { event = "cmdgetip", name = ci.name, cn = ci.clientnum, location = c(location), country = c(country), contime = contime, ip = tostring(ip.ip(ci)) }
  sendUDP(res) 
end, "<cn>", "Display IP of player <cn>.")

addcommand("help", function(info) 
  local list, tmp, dontshow, n = {}, {}, {}, 0
  for k, v in pairs(module.config.commands) do table.insert(tmp, k) end
  for _, c in ipairs(table.sort(tmp)) do if not dontshow[c] then 
    local cmd = module.config.commands[c]
    if cmdaliases[c] then 
      for _, a in ipairs(cmdaliases[c]) do if not dontshow[a] then dontshow[a] = true end end
      c = table.concat(cmdaliases[c], "/")
    end
    n = n + 1
    list[n] = { name = c, cmdargs = cmd.cmdargs, help = cmd.help}
  end end
  local res = { event = "cmdhelp", server = cs.serverdesc, list = list }
  sendUDP(res) 
end, nil, "View bot commands and functionality.")

addcommand({"disc", "disconnect"}, function(info)
  local cns, cnerr = {}, false
  for cn in info.args:gmatch("%d+") do table.insert(cns, tonumber(cn)) end
  if not next(cns) then return cmderror(info, "You need to enter which cns to disconnect.") end
  for _, cn in ipairs(cns) do
    who = engine.getclientinfo(cn)
    if who and who.clientnum then
      engine.writelog(("disconnected %s (%d) by %s"):format(who.name, who.clientnum, info.user))
      engine.disconnect_client(who.clientnum, engine.DISC_MAXCLIENTS)
    else cnerr = true end
  end 
  if cnerr then cmderror(info, "At least one cn you entered could not be disconnected.") end
end, "<cn> [<cn> <cn>...]", "Disconnect player(s) <cn> sending the message 'server FULL'.")

addcommand("syncserver", function(info)
  if not module.config.host or not module.config.port or not module.config.channel then
    return cmderror(info, "No configuration found. Please provide the main config and restart the server.")
  end
  createUDP(module.config.host, module.config.port, cs.serverport, module.config.channel, module.config.scoreboardChannel)
end, nil, "Refreshes UDP connection of gameserver '" .. cs.serverdesc .. "'; clears status channel.")

addcommand("mute", function(info)
  if not info.args or info.args == "" then return cmderror(info, "You need to enter a cn.") end
  local who = engine.getclientinfo(tonumber(info.args) or -1)
  if not who then return cmderror(info, "Player not found.") end
  local muted = who.extra.ipextra.muted
  if muted then spaghetti.cancel(muted) end
  local _ip = ip.ip(ciip(who.clientnum))
  who.extra.ipextra.muted = spaghetti.later(30 * 60 * 1000, function()
    local extra = ipextra.find(_ip)
    if extra then
      extra.muted = nil
      engine.writelog("mute: expire " .. tostring(ip.ip(_ip)))
    end
  end)
  local txt = ("Muted %s (%d) for one hour."):format(who.name, who.clientnum)
  local res = { event = "info", txt = txt }
  sendUDP(res) 
  engine.writelog("Discord: " .. info.user .. " muted " .. tostring(ip.ip(_ip)))
end, "<cn>", "Mute a player for 1 hour.")

addcommand("unmute", function(info)
  if not info.args or info.args == "" then return cmderror(info, "You need to enter a cn.") end
  local who = engine.getclientinfo(tonumber(info.args) or -1)
  if not who then return cmderror(info, "Player not found.") end
  local _ip, extra = ip.ip(ciip(who.clientnum)), who.extra.ipextra
  local muted = extra.muted
  if not muted then return cmderror(info, "This player is not muted.") end
  spaghetti.cancel(muted)
  extra.muted = nil
  local txt = ("Unmuted %s (%d)."):format(who.name, who.clientnum)
  local res = { event = "info", txt = txt }
  sendUDP(res) 
  engine.writelog("Discord: " .. info.user .. " unmuted " .. tostring(ip.ip(_ip)))
end, "<cn>", "Unmute a player.")

local timespec = { d = { m = 60*60*24, n = "days" }, h = { m = 60*60, n = "hours" }, m = { m = 60, n = "minutes" } }
timespec.D, timespec.H, timespec.M = timespec.d, timespec.h, timespec.m
local banlists = {["kick"] = true, ["openmaster"] = true, ["teamkill"] = true}
local function kickban(info, pban)
  local force, who, name, time, mult, msg
  if pban then force, who, name, msg = info.args:match("^(!?)([%d%./]+)%s*([^%s]*)%s*(.-)%s*$")
  else force, who, name, time, mult, msg = info.args:match("^(!?)([%d%./]+)%s*([^ %d]*)%s*(%d*)([DdHhMm]?)%s*(.-)%s*$") end
  local cn, _ip = tonumber(who), ip.ip(who or "")
  force, list, msg = force == "!", name == "" and "kick" or name, msg ~= "" and msg or nil
  if (not cn and not _ip) or (not _ip and force) or (not pban and (not time or (time ~= "" and not timespec[mult]))) then return cmderror(info, "Bad format.") end
  if not pban then 
    if time == "" then time, msg = 4*60*60, mult ~= "" and msg and mult .. " " .. msg or msg
    elseif time == '0' then return cmderror(info, "Cannot ban for no time.")
    else time = timespec[mult].m * time end
  else time = nil end
  if not banlists[list] then return cmderror(info, "Ban list '" .. list .. "' not found.") end
  _ip = _ip or ip.ip(ciip(cn))
  if cn and _ip.ip == 0 then return cmderror(info, "Player not found.") end
  if cn and engine.getclientinfo(cn).privilege >= server.PRIV_ADMIN then return cmderror(info, "The player has sufficient credentials to bypass the ban, not adding.") end
  local found, contained, matched1, matched2 = false, false, {}, {} 
  for ban, expire, msg in ban.enum(list) do 
    if ban:matches(_ip) then found, matched1[ban], contained = true, true, ip.ip(ban).mask < _ip.mask
    elseif _ip:matches(ban) then found, matched2[ban] = true, true end
  end
  if found then 
    local failmsg
    if next(matched1) and contained then failmsg = "it is already contained by " .. tostring(next(matched1))
    elseif next(matched1) and next(matched1) == _ip then failmsg = not force and "it is already present" 
    elseif next(matched2) and next(matched2) ~= _ip then failmsg = not force and "it contains ranges " .. table.concat(map.lp(tostring, matched2), ", ") .. ".\nPrepend a '!' to coalesce present ranges" end
    if failmsg then cmderror(info, "Not adding ban because " .. failmsg .. ".") return end
    if next(matched1) then for range in pairs(matched1) do ban.unban(list, range, true) end
    elseif next(matched2) then for range in pairs(matched2) do ban.unban(list, range, true) end end
  end
  local admin = "Remote Admin '" .. info.user .. "'"
  local kicked = pick.sf(function(ci) return _ip:matches(ip.ip(ci)) and (not (ci.privilege >= server.PRIV_ADMIN)) end, iterators.clients())
  for p in pairs(kicked) do p.extra.iskicked = true end
  ban.ban(list, _ip, msg, time, nil, engine.decodeutf8(admin))
  local res = { event = "info", txt = ("Added a ban on %s in banlist '%s'."):format(tostring(_ip), list) }
  sendUDP(res)
  if not next(kicked) then return end
  local res = { event = "kick", str = ("%s bans %s"):format(c(admin), tostring(_ip)) }
  sendUDP(res)
end
addcommand(
  {"ban", "kick"}, 
  kickban, 
  "<cn/[!]range> [<list=kick>] <time> [<reason>]", 
  "Ban an IP(range)/a player <cn>. Time format: #d|#h|#m\nPrepending '!' will coalesce all present containing ranges."
)
addcommand(
  {"permban", "pban"}, 
  function(info) return kickban(info, true) end, 
  "<cn/[!]range> [<list=kick>] [<reason>]", 
  "Permanently ban an IP(range)/a player <cn>.\nPrepending '!' will coalesce all present containing ranges."
)

addcommand("unban", function(info)
  local force, who, name = info.args:match("^(!?)([%d%./]+)%s*([^%s]*)%s*$")
  local _ip = ip.ip(who or "")
  name, force = name == "" and "kick" or name, force == "!"
  if not _ip then return cmderror(info, "Bad format.") end
  if not banlists[name] then return cmderror(info, "Ban list '" .. name .. "' not found.") end
  local found, contained, matched1, matched2 = false, false, {}, {} 
  for ban, expire, msg in ban.enum(name) do 
    if ban:matches(_ip) then found, matched1[ban], contained = true, true, ip.ip(ban).mask < _ip.mask
    elseif _ip:matches(ban) then found, matched2[ban] = true, true end
  end
  local failmsg
  if not found then failmsg = "it is not in banlist '" .. name .. "'"
  elseif next(matched1) and contained then failmsg = "it would still be contained by " .. tostring(next(matched1)) 
  elseif next(matched2) and next(matched2) ~= _ip then failmsg = not force and "it contains ranges " .. table.concat(map.lp(tostring, matched2), ", ") .. ".\nPrepend a '!' to force-remove all included ranges" end
  if failmsg then cmderror(info, "Not deleting ban because " .. failmsg .. ".") return end
  if next(matched1) then for range in pairs(matched1) do ban.unban(name, range) end
  elseif next(matched2) then for range in pairs(matched2) do ban.unban(name, range) end end
  local res = { event = "info", txt = ("Deleted the ban on %s from banlist '%s'."):format(tostring(_ip), name) }
  sendUDP(res)
end, "<[!]range> [<list=kick>]", "Unban an IP(range) [from banlist].\nIf !forced, removes all included ranges.")

local function fetchbans(list)
  local bans, hasbans = {}, false
  for ip, expire, msg in ban.enum(list) do
    if not hasbans then hasbans = true end
    if not msg or msg == "" or msg == "your ip is banned" or msg == "you teamkill too much" then msg = "--" end
    table.insert(bans, ("%s :: %s :: %s"):format(unixprint(expire), tostring(ip), c(msg) or ""))
  end
  if not hasbans then table.insert(bans, "Banlist empty.") end
  return bans
end

addcommand("banlist", function(info)
  local list, lists, blist, istrimmed, all = info.args:match("([^ %d]*)"), {}, {}, false, false
  if list and list ~= "" and not banlists[list] then return cmderror(info, "Ban list '" .. list .. "' not found.") end
  if list and list ~= "" then
    local lstr
    local bans = fetchbans(list)
    lstr, istrimmed = trim(table.concat(bans, "\n"))
    blist = { name = list, list = lstr }
  else
    all = true
    local n = 0
    for l in pairs(banlists) do
      n = n + 1
      local bans = fetchbans(l)
      local lstr, trimmed = trim(table.concat(bans, "\n"), 800)
      istrimmed = trimmed
      lists[n] = { name = l, list = lstr }
    end
  end
  local res = { event = "cmdbanlist", all = all, lists = lists, blist = blist }
  sendUDP(res)
  if not istrimmed then return end
  local txt = "At least one banlist is too long to display and had to be trimmed in length.\nCheck " .. info.prefix .. "banlist <list> for a more complete enumeration."
  local res = { event = "info", txt = txt }
  sendUDP(res)
end, "[<list>]", "List bans from all or one of the following lists: kick, openmaster, teamkill")

--[[
    #cheater command
]]

local function discordnotify(args)
  local str, reporters, num, more = "", "", 0, false
  local con = module.config.connected
  if not con then return end
  for _ip, requests in pairs(args) do
    num = num + 1
    local names
    str = str .. (num > 1 and "==\n" or "\n") .. "IP: " .. tostring(ip.ip(_ip))
    for cheater in pairs(requests.cheaters) do 
      str, names = str .. "\n" .. playerStats(cheater, nil, true, server.m_ctf or server.m_hold), true 
    end
    if not names then str = str .. "\n<disconnected>" end
    str = str .. "\n"
    local multi
    for reporter in pairs(requests.reporters) do 
      reporters = reporters .. (more and ", " or "") .. prettyname(reporter)
      multi, more = true, true			
    end
    if not multi then reporters = reporters .. "<disconnected>" end
    reporters = reporters .. " (" .. requests.total .. " request".. (requests.total > 1 and "s" or "") .. ")"
  end
  str = "Suspect" .. (num > 1 and "s" or "") .. ": \n" .. str 
  local res = { event = "alert", server = cs.serverdesc, subject = str, reporters = c(reporters) }
  sendUDP(res)
end

require"std.abuse".cheatercmd(discordnotify, 20000, 1/30000, 3)


-- export some utils
function module.c(msg) return c(msg) end
function module.new(info) return new(info) end
function module.sendUDP(msg) return sendUDP(msg) end
function module.playerStats(ci, n, includename, includeflags) return playerStats(ci, n, includename, includeflags) end
function module.addcommand(cmd, fn, cmdargs, help) return addcommand(cmd, fn, cmdargs, help) end

return module

