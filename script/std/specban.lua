--[[
      specban.lua: Implement IP-based specbanning, featuring optional time durations, reasons and a specbanlist
      Author: benzomatic
--]]

local playermsg, commands, iterators = require"std.playermsg", require"std.commands", require"std.iterators"
local ip, fp, L = require"utils.ip", require"utils.fp", require"utils.lambda"
local map, pick = fp.map, fp.pick

local unixtime = os.time
local specbans = {}
local max = 4 * 60 * 60 -- 4 hours max
local timeinfo = {
  h = { t = 60*60, n = "hours" }, 
  m = { t = 60, n = "minutes" } 
}


--[[
    Main functionality
--]]

local function prettytime(time)
  local tbl, msg = os.date("!*t", time), ""
  if tbl.hour ~= 0 then msg = msg .. " " .. tbl.hour .. "h" end
  if tbl.min ~= 0 then msg = msg .. " " .. tbl.min .. "m" end
  if tbl.sec ~= 0 then msg = msg .. " " .. tbl.sec .. "s" end
  return msg
end

local function getauthuser(allclaims)
  if not allclaims or not next(allclaims) then return nil end
  for k, v in pairs(allclaims) do if v == true then return tostring(k) end return nil end
end

local function explain(ci, ban)
  playermsg("\n\f6Info\f7: You are locked to spectator mode. \f6Remaining\f7:" .. prettytime(ban.expire - unixtime()) .. " || \f6Master\f7: " .. ban.master .. " || \f6Reason\f7: " .. ban.reason .. "\n\f6Info\f7: \f7Ask an auth holder for assistance.", ci)
end

local function split(str)
  if not str then return nil, nil, nil end
  local reason = ""
  local time, mult = str:match("^(%d+)([mh])%s")
  if time and mult then 
    reason = str:gsub("(%d+)([mh])%s", "")
    return time, mult, reason
  else
    return (time and time or nil), (mult and mult or nil), str
  end
end

local function delspecban(ipban)
  local _ip = tostring(ipban)
  local ban = specbans[_ip]
  if not ban then return false end
  spaghetti.cancel(specbans[_ip].keep)
  specbans[_ip] = nil
  engine.writelog("specban: " .. _ip .. " lifted")
  local banned = pick.sf(function(cl) return ip.ip(ipban):matches(ip.ip(cl)) end, iterators.clients())
  if banned then 
    for ci in pairs(banned) do 
      server.unspectate(ci)
      playermsg("\f6Info\f7: Your specban has been lifted.", ci) 
    end 
  end
  return true
end

local function addspecban(ipban, reason, expire, master)
  local banned = pick.sf(function(cl) return ip.ip(ipban):matches(ip.ip(cl)) end, iterators.clients())
  local who = next(banned) and table.concat(map.lp(L"server.colorname(_, nil)", banned), "\f6/\f7") or "\f6---\f7"
  local keep = spaghetti.later(expire * 1000, function() delspecban(ipban) end)
  local expire = expire and unixtime() + expire or unixtime() + max
  specbans[ipban] = {names = who, reason = reason, expire = expire, master = master, keep = keep}
  if not specbans[ipban] then return false end
  engine.writelog("specban: " .. ipban .. " added")
  if banned then
    for ci in pairs(banned) do 
      server.forcespectator(ci) 
      explain(ci, specbans[ipban])
    end
  end
  return true
end

local function checkban(ipban)
  return specbans[tostring(ipban)]
end


--[[
    Commands
--]]

commands.add("specbans", function(info) 
  if info.ci.privilege < server.PRIV_AUTH then playermsg("Access denied.", info.ci) return end
  if not next(specbans) then playermsg("\f6Info\f7: No specbans found.", info.ci) return end
  for sb, ban in pairs(specbans) do
    playermsg("\f6Specban\f7: " .. sb .. " || \f6On\f7: " .. ban.names .. " || \f6Remaining\f7:" .. prettytime(ban.expire - unixtime()) .. " || \f6Master\f7: " .. ban.master .. " || \f6Reason\f7: " .. ban.reason, info.ci)
  end
end, "#specbans: List all specbans.")

local help1 = "#delspecban <cn|ip>: Delete a specban by cn or IP."
commands.add("delspecban", function(info) 
  if info.ci.privilege < server.PRIV_AUTH then playermsg("Access denied.", info.ci) return end
  local who = info.args:match("^[%d%./]+$")
  if not who then return playermsg("\f6Info\f7: Invalid arguments. Format: " .. help1, info.ci) end
  local cn, _ip = tonumber(who), ip.ip(who or "")
  _ip = _ip or ip.ip(engine.ENET_NET_TO_HOST_32(engine.getclientip(cn)))
  if cn and _ip.ip == 0 then return playermsg("\f6Info\f7: Specban not found.", info.ci) end
  if not delspecban(tostring(_ip)) then
    playermsg("\f6Info\f7: Specban '" .. tostring(_ip) .. "' not found.", info.ci)
  else
    playermsg("\f6Info\f7: Deleted specban for '" .. tostring(_ip) .. "'.", info.ci)
  end
end, help1)

local help2 = "#specban <cn|ip> [time=30m] <reason>: Add a specban by cn or IP. \n\f6Info\f7: Time is optional, reason necessary."
commands.add("specban", function(info) 
  if info.ci.privilege < server.PRIV_AUTH then playermsg("Access denied.", info.ci) return end 
  local who, more = info.args:match("^([%d%./]+)%s+(.*)")
  if not who then return playermsg("\f6Info\f7: Invalid arguments. Format: " .. help2, info.ci) end
  local time, mult, reason = split(more) 
  local cn, _ip = tonumber(who), ip.ip(who or "")
  if (not cn and not _ip) or not reason or (reason == "") then 
    return playermsg("\f6Info\f7: Invalid arguments. Format: " .. help2, info.ci) 
  end
  _ip = _ip or ip.ip(engine.ENET_NET_TO_HOST_32(engine.getclientip(cn)))
  local _time, toolong = 30 * 60, false
  if mult and time then _time = timeinfo[mult].t * time end
  if _time > max then _time, toolong = max, true end
  if cn and _ip.ip == 0 then return playermsg("\f6Info\f7: Client not found.", info.ci) end
  delspecban(tostring(_ip))
  local master = getauthuser(info.ci.extra.allclaims[cs.serverauth]) or info.ci.name  -- use local auth name if it exists
  if not addspecban(tostring(_ip), reason, _time, master) then
    playermsg("\f6Info\f7: Could not add specban for '" .. tostring(_ip) .. "'.", info.ci)
    return
  else
    playermsg("\f6Info\f7: Added specban for '" .. tostring(_ip) .. "'. Duration: " .. (toolong and "4 hours (the timeframe you entered was too long)." or (time and (tostring(time) .. " " ..  (mult and timeinfo[mult].n or "")) or "30 minutes") .. "."), info.ci)
  end
end, help2)


--[[
    Hooks
--]]

spaghetti.addhook(server.N_SPECTATOR, function(info) 
  local ip = ip.ip(engine.ENET_NET_TO_HOST_32(engine.getclientip(info.ci.clientnum)))
  local ban = checkban(ip)
  if ban then
    info.skip = true
    explain(info.ci, ban)
  end
end)

spaghetti.addhook("connected", function(info) 
  local ip = ip.ip(engine.ENET_NET_TO_HOST_32(engine.getclientip(info.ci.clientnum)))
  local ban = checkban(ip)
  if ban then
    server.forcespectator(info.ci)
    explain(info.ci, ban)
  end
end)

