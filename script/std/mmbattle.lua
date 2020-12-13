--[[
    Create a colorful little multi mode and map battle, as seen on the flagrun server.
    Set the mode/map pool with require"std.mmbattle".pool({ modes = { mode1, mode2.. }, maps = { map1, map2.. }, count = choice_amount })
]]

local playermsg, commands, iterators = require"std.playermsg", require"std.commands", require"std.iterators"
local map, L = require"utils.fp".map, require"utils.lambda"

local choices, suggested, battle, winnermode, winnermap = {}, {}, false

local colors = { "\f0", "\f1", "\f2", "\f3", "\f5", "\f6" }
local defaultpool, activepool = { modes = { 12, 17 }, maps = { "forge", "reissen", "haste", "dust2" }, count = 3 }
local function pool(custom) activepool = custom or defaultpool end

local function randommode() return activepool.modes[math.random(#activepool.modes)] end
local function randommap()
  local maps, existingmaps, tries, rndmap = activepool.maps, map.sp(L"_2.map", choices), 4
  repeat rndmap, tries = maps[math.random(#maps)], tries + 1 until (not existingmaps[rndmap] and server.smapname ~= rndmap) or tries >= 10
  return rndmap
end
local function randomcolor(colordb) return next(colordb) and table.remove(colordb, math.random(#colordb)) or "\f7" end

local function getchoices()
  choices = {}
  local colordb = map.li(L"_2", colors)
  map.lft(suggested, L"_.extra.mmvoted", iterators.select(L"_.extra.mmvoted"))
  for n = 1, activepool.count do
    local newchoice = next(suggested) and table.remove(suggested) or { mode = randommode(), map = randommap() }
    newchoice.color, newchoice.votes = randomcolor(colordb), 0 
    choices[n] = newchoice
  end
  return choices
end

local function initbattle()
  choices, str, battle = getchoices(), {}, true
  for i = 1, activepool.count do table.insert(str, choices[i].color .. tostring(i)) end
  server.sendservmsg("Vote for the next mode/map with " .. table.concat(str, "\f7, ") .. "\f7:")
  server.sendservmsg("\t" .. table.concat(map.li(L"_2.color .. _ .. ' \fs\f7- \fr'.. server.modename(_2.mode, '?') .. ' on ' .. _2.map", choices), "  \f7|  ") .. "\n")
end

local function resolvebattle()
  local winner, realwinner = nil
  for ci in iterators.select(L"_.extra.mmvote") do
    local choice = choices[ci.extra.mmvote]
    choice.votes = choice.votes and choice.votes + 1
  end
  for _, choice in pairs(choices) do if not winner or choice.votes > winner.votes then winner = choice end end
  if winner and winner.votes == 0 then winner = choices[math.random(#choices)] end
  choices, battle = {}, false
  if not winner then return end
  winnermode, winnermap, realwinner = winner.mode, winner.map, winner.votes > 0
  server.sendservmsg((realwinner and "\n" or "") .. "\t\t->\t" .. (realwinner and "Winner: " or "Selecting ") .. winner.color .. server.modename(winner.mode, '?') .. " on " .. winner.map .. (realwinner and " \f7(" .. winner.votes .. " vote" .. (winner.votes ~= 1 and "s" or "") .. ")" or ""))
end

spaghetti.addhook(server.N_TEXT, function(info)
  if info.skip or not battle or not choices[tonumber(info.text)] then return end
  info.skip, info.ci.extra.mmvote = true, tonumber(info.text)
  local choice = choices[tonumber(info.text)]
  server.sendservmsg("\t->\t" .. choice.color .. server.modename(choice.mode, '?') .. " on " .. choice.map .. " \f7voted by " .. info.ci.name)
end)

spaghetti.addhook(server.N_MAPVOTE, function(info)
  if info.skip then return end                
  info.ci.extra.mmvoted = { mode = tonumber(info.reqmode), map = info.text }
  playermsg("Your map suggestion will appear during the next mapbattle.", info.ci)
end)

spaghetti.addhook("intermission", function(info)
  if not next(activepool) then engine.writelog("Please provide modes and maps for the mapbattle pool. Module disabled.") return end
  if not activepool.count then activepool.count = 3 end
  server.interm = server.interm + 11000
  spaghetti.latergame(5000, initbattle)
  spaghetti.latergame(18000, resolvebattle)
end)

spaghetti.addhook("prechangemap", function(info) if winnermode and winnermap then info.mode, info.map = winnermode, winnermap end end)

spaghetti.addhook("changemap", function(info)
  choices, suggested, battle, winnermode, winnermap = {}, {}, false, nil, nil
  for ci in iterators.all() do ci.extra.mmvote, ci.extra.mmvoted = nil, nil end
end)

return { pool = pool }
