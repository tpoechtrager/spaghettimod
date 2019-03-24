--[[
    autobalance.lua:   - implement automatic player count balance
                       - use soft and hard balance of bottom players, never switch flagholders
]]

local playermsg, iterators, commands, setteam = require"std.playermsg", require"std.iterators", require"std.commands", require"std.setteam"
local L = require"utils.lambda"

local maxdiff = 1  -- the maximum player unbalance that is tolerated, everything beyond this will be autobalanced
local enable_balance, enable_hardbalance = true, true
local checkteam_interval = 40 * 1000
local softbalance_duration = 20 * 1000

local candidates = {}  -- players to be balanced

local function teams()
  local info, good_n, evil_n = {}, 0, 0
  for p in iterators.inteam("good") do if (p.state.state ~= engine.CS_SPECTATOR) then good_n = good_n + 1 end end
  for p in iterators.inteam("evil") do if (p.state.state ~= engine.CS_SPECTATOR) then evil_n = evil_n + 1 end end
  info.good = { a = "evil", n = good_n }
  info.evil = { a = "good", n = evil_n }
  info.diff = math.abs(good_n - evil_n)
  info.bigger = good_n > evil_n and "good" or evil_n > good_n and "evil" or nil
  info.smaller = good_n < evil_n and "good" or evil_n < good_n and "evil" or nil
  return info
end

local function unbalanced()
  local teaminfo = teams()
  return teaminfo.diff > maxdiff
end

local function has_flag(ci)
  local flags, actorflags = server.ctfmode.flags, {}
  for i = 0, flags:length() - 1 do if flags[i].owner == ci.clientnum then actorflags[i] = true end end
  return next(actorflags)
end

local function determine_worst_players(team, num)
  local n, t, worst = 0, {}, {}
  for ci in iterators.inteam(team) do if (ci.state.state ~= engine.CS_SPECTATOR) then
    local points = ci.state.frags + ci.state.flags * 100
    t[ci.clientnum] = points 
  end end
  if not next(t) then return worst end
  local worstcn, worstpoints = nil, 10000
  repeat
    for cn, points in pairs(t) do 
      if points < worstpoints then 
        worstcn, worstpoints = cn, points 
      end 
    end
    if worstcn == nil then break end
    local player = engine.getclientinfo(worstcn)
    worst[worstcn] = teams()[player.team].a
    t[worstcn] = nil
    n = n + 1
  until n >= num or not next(t)
  return worst
end

local function updatecandidates()
  if not enable_balance then return end
  local teaminfo = teams()  
  if candidates then 
    for k, v in pairs(candidates) do 
      candidates[k] = nil
    end
    candidates = nil
  end
  local candidate_num = math.floor(teaminfo.diff / 2)
  candidates = determine_worst_players(teaminfo.bigger, candidate_num)
end

local function tryswitchteam(player, toteam)
  local teaminfo = teams()  
  if not unbalanced() or toteam == teaminfo.bigger then return end
  if has_flag(player) then return end  -- just making sure..
  local oldgood, oldevil = teaminfo.good.n, teaminfo.evil.n
  setteam.set(player, toteam, -1, true)
  teaminfo = teams()
  engine.writelog("autobalance: teams: good/evil: " .. oldgood .. "/" .. oldevil .. " => " .. teaminfo.good.n .. "/" .. teaminfo.evil.n .. " ["  .. player.name .. " (" .. player.clientnum .. ")]")
  playermsg("\f6Info\f7: You were switched into the other team to balance out the player count.", player)
end

-- hard balance: switch players into the other team while they are alive - the players will remain alive. 
-- only applies when soft balance is not fast enough, only switches bottom players and never switches flagholders.
local function hardbalance()
  if not unbalanced() then return end
  updatecandidates()
  for cn, team in pairs(candidates) do
    local p = engine.getclientinfo(cn)
    if not has_flag(p) then tryswitchteam(p, team) end
  end 
end

local diedhook
local function balanceteams()
  if not unbalanced() then return end
  updatecandidates()
  
  -- soft balance: switch players that are currently dead
  for player in iterators.select(L"_.state.state == engine.CS_DEAD") do 
    if candidates[player.clientnum] then 
      tryswitchteam(player, candidates[player.clientnum])
      updatecandidates()
    end
  end

  -- soft balance: switch players that will die within the next softbalance_duration seconds
  diedhook = spaghetti.addhook("servmodedied", function(info) 
    local cn = info.target.clientnum
    if not candidates[cn] or not unbalanced() then return end
    spaghetti.later(1000, function()  -- delay this so we don't switch people who only went into spec
      local c = engine.getclientinfo(cn)
      if c.state.state == engine.CS_SPECTATOR then 
        updatecandidates()
        return 
      end
      tryswitchteam(c, candidates[c.clientnum])
      updatecandidates()
    end)
  end)

  spaghetti.later(softbalance_duration, function() 
    spaghetti.removehook(diedhook)
    if unbalanced() then if enable_hardbalance then hardbalance() end end  -- if still unbalanced, apply hard balance
  end)
end

-- check balance every checkteam_interval seconds
local checker
if enable_balance then 
  checker = spaghetti.later(checkteam_interval, function() balanceteams() end, true)
end

-- commands to toggle things
commands.add("abtoggle", function(info)
  if info.ci.privilege < server.PRIV_ADMIN then playermsg("Access denied.", info.ci) return end
  enable_balance = not enable_balance
  if enable_balance then 
    if checker then spaghetti.cancel(checker) end
    checker = spaghetti.later(checkteam_interval, function() balanceteams() end, true)
  else
    if checker then spaghetti.cancel(checker) end
  end
  playermsg("\f6Info\f7: Auto-balance is now " .. (enable_balance and "enabled" or "disabled") .. ".", info.ci)
end)

commands.add("abmaxdiff", function(info)
  if info.ci.privilege < server.PRIV_ADMIN then playermsg("Access denied.", info.ci) return end
  local val = tonumber(info.args) or nil
  if val then   
    maxdiff = val
  end
  playermsg("\f6Info\f7: The maximum tolerated teamsize difference is " .. maxdiff .. ".", info.ci)
end)

