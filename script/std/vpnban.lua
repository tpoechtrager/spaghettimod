--[[
    Ban VPNs with the free http://iphub.info API - for it to work, an API key is needed.
    Creates a new "vpnbans" banlist as well as a "goodips" cache in order to keep API requests to a minimum. 
    All VPN bans can be bypassed with an authkey that has the "vpn-bypass" domain.
]]

local auth, ban, playermsg = require"std.auth", require"std.ban", require"std.playermsg"
local ip, jsonpersist, servertag = require"utils.ip", require"utils.jsonpersist", require"utils.servertag"
local http, ltn12, json = require"socket.http", require"ltn12", require"dkjson"

local API_KEY = ""
local bannedblocks = { 
--[0] = true, -- bans residential/unclassified IP (those are the good guys)
--[2] = true, -- bans non-residential & residential IP (may flag innocent people)
  [1] = true  -- bans non-residential IP (hosting provider, proxy, etc.)
}

local ok = API_KEY and API_KEY ~= ""
if not ok then return engine.writelog("Cannot load the VPNBAN module: Missing API Key.") end

local function iplookup(lookupip)
  if not lookupip or lookupip == "" then return nil end
  local resp = {}
  local status, code = http.request{
    url = "http://v2.api.iphub.info/ip/" .. lookupip,
    headers = {["X-Key"] = API_KEY},
    sink = ltn12.sink.table(resp)
  }
  if not status then engine.writelog("VPNBAN ERROR: Cannot fetch IP: " .. lookupip) return nil end
  local tbl = json.decode(resp[1], 1, nil)
  if not tbl then engine.writelog("VPNBAN ERROR: Cannot fetch IP: " .. lookupip) return nil end
  if tbl.error then engine.writelog("LOOKUP ERROR: [Code " .. code .. "] " .. lookupip .. ": " .. tbl.error)
  else engine.writelog("VPNBAN FAIL: [Code " .. code .. "] IP lookup could not be completed.") end
  return status and code and code == 200, tbl.block, tbl.ip
end

-- persist whitelist
local goodips = jsonpersist.load(servertag.fntag .. "goodips") or {}
local function persistip(load)
  jsonpersist.save(goodips, servertag.fntag .. "goodips") 
  for k, v in pairs(goodips) do goodips[k] = nil end
  if not load then return end
  goodips = jsonpersist.load(servertag.fntag .. "goodips")
end
spaghetti.addhook("changemap", function(info) persistip(true) end)
spaghetti.addhook("shuttingdown", function(info) if not info.servererror then persistip() end end)

-- check client connects for chosen blocks
spaghetti.addhook("enterlimbo", function(info) 
  local bans = ban.checkban(info.ci)
  if next(bans) then return end
  local _ip = tostring(ip.ip(info.ci))
  if goodips[_ip] then return end
  local s, b, i = iplookup(_ip)
  if not s then return engine.writelog("VPNBAN ERROR: IP Lookup failed.") end
  if bannedblocks[b] then 
    ban.ban("vpnban", ip.ip(_ip))
  else 
    if not goodips[i] then goodips[i] = { net = i, block = b } end
  end
end, true)

spaghetti.addhook("commands.banenum", function(info)
  if info.args:match"^[^ ]*" == "vpnban" then info.skip = true return playermsg("The vpn-banlist cannot be enumerated in-game.", info.ci) end
end, true)

ban.newlist(
  "vpnban", 
  "Your IP appears to be a proxy - only residential IPs can play on this server", 
  { client = server.PRIV_ADMIN, bypass = { server.PRIV_AUTH, ["vpnban-bypass"] = true } },
  servertag.fntag .. "vpnbans"
)

table.insert(auth.preauths, "vpnban-bypass")

-- clear cache bi-weekly
local cache = engine.totalmillis
spaghetti.addhook("noclients", function()
  if (engine.totalmillis - cache) >= 3 * 24 * 60 * 60 * 1000 then 
    ban.clear("vpnban") 
    for k, v in pairs(goodips) do goodips[k] = nil end
    jsonpersist.save({}, servertag.fntag .. "goodips")
    cache = engine.totalmillis
  end
end)


return module

