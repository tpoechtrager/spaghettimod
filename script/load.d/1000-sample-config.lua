--[[

  This is the configuration I use on my server.

]]--

if not os.getenv("SPAGHETTI") then return end
engine.writelog("Applying the sample configuration. Port: 28785")

local servertag = require"utils.servertag"
servertag.tag = "spaghetti"

local uuid = require"std.uuid"

local fp, L = require"utils.fp", require"utils.lambda"
local map, I = fp.map, fp.I
local abuse, playermsg = require"std.abuse", require"std.playermsg"

cs.maxclients = 20
cs.serverport = 28785

cs.updatemaster = 1
spaghetti.later(10000, L'engine.requestmaster("\\n")', true)
spaghetti.addhook("masterin", L'if _.input:match("^failreg") then engine.lastupdatemaster = 0 end', true)

--make sure you delete the next two lines, or I'll have admin on your server.
cs.serverauth = "spaghetti"
local auth = require"std.auth"
cs.adduser("benzomatic", "spaghetti", "+a26e607b5554fd5b316a4bdd1bfc4734587aa82480fb081f", "a")
table.insert(auth.preauths, "spaghetti")


spaghetti.addhook(server.N_SETMASTER, L"_.skip = _.skip or (_.mn ~= _.ci.clientnum and _.ci.privilege < server.PRIV_AUTH)")

cs.serverdesc = ":: spaghettimod ::"

cs.lockmaprotation = 2
cs.ctftkpenalty = 0
cs.maprotationreset()


--copied from data/menus.cfg
local ffamaps, capturemaps, ctfmaps = table.concat({
  "aard3c abyss academy access albatross akaritori akimiski alithia alloy antel anubis aod aqueducts arbana asenatra asthma averas",
  "awoken bvdm_01 carbide cartel castle_trap catacombs cavefire church51 clash collusion colony complex  conflict corruption crypta", 
  "curvedm curvy_castle darkdeath deathtek depot dirtndust dispute DM_BS1 dock dopamine douze duel5 duel7 duel8 dune elegy exist exo", 
  "fallen fanatic_quake fdm6 ferguson force frag-lab frag2 fragplaza frostbyte frozen fubuki fury ghetto gorge gothic-df guacamole gubo", 
  "hades hashi hator haze hdm3 headroom helligsted hektik hillfort hog2 horus idyll3 imhotep industry infernal injustice insipid island", 
  "janela justice kalking1 kastro katrez_d kffa killfactory kmap5 konkuri-to ksauer1 legacy legazzo lost_soul lost_world lostinspace maple",
  "masdm mbt2 mbt9 mbt10 memento memoria metl2 metl3 metl4 mood moonlite neondevastation neonpanic nessus nmp8 nmp10 nucleus oasis oddworld",
  "ognjen ogrosupply oldschool orbe orion osiris ot outpost paradigm pariah park pgdm phosgene phrantic pitch_black powerplant purgatory", 
  "refuge renegade rm1 rm5 roughinery ruby ruine saffier sandstorm sauerowalk sauerstruck sdm1 shadowed shindou shinmei1 shiva simplicity",
  "skrdm1 skycastle-r slingshot souls spcr2 stahlbox stemple stronghold suburb suisei tartech teahupoo tejen thetowers thor torment toxicity", 
  "tumwalk turbine turmoil unworld unworld2 ventania waltz wake5 wdcd zamak zavial zdm2 ztn"
}, " "), table.concat({
  "aastha abbey access akimiski akroseum alithia anubis aod arabic asenatra asgard asteroids averas bklyn c_egypt c_lone c_valley",
  "campo capture_night caribbean casa collide collusion core_refuge core_transfer corruption croma cwcastle damnation destiny dirtndust",
  "disc disruption donya duomo dust2 earthstation eris eternal_valley evilness face-capture fallen fb_capture fc3 fc4 fc5 forge fragnostic",
  "frostbyte fusion genesis ghetto gorge gothic-df hades hallo harbor haste hidden imhotep infamy infernal killcore3 kopenhagen laucin",
  "lostinspace luna mbt12 mc-lab meltdown2 mercury metro monastery nevil_c new_energy nitro nmp4 nmp9 nucleus ogrosupply overdrive ow pandora",
  "paradigm pariah ph-capture pul1ctf reissen relic risk river_c river_keep ruby ruebli rust serenity skycastle-r snapper_rocks spcr stadium",
  "stronghold subterra suburb surge tempest tortuga triforts turbulence turmoil twinforts urban_c valhalla venice waltz xenon zamak"
}, " "), table.concat({
  "aastha abbey akimiski akroseum arbana asgard authentic autumn bad_moon berlin_wall bklyn breakout bt_falls campo capture_night casa",
  "catch22 collide core_refuge core_transfer croma ctf_suite daemex damnation desecration destiny disc disruption divine donya duomo dust2",
  "earthsea earthstation enigma eris eternal_valley europium evilness face-capture fc4 fc5 fire_keep flagstone forge forgotten fortress", 
  "fragnostic fusion garden gubo hallo harbor haste hidden idris infamy kiryu kopenhagen l_ctf laucin luna mach2 mbt1 mbt4 mbt12 mc-lab", 
  "meltdown2 mercury metro mill new_energy nitro nucleus overdrive ow pandora pul1ctf ra recovery redemption regal reissen risk river_keep",
  "ruebli rust sacrifice shellshock2 shipwreck siberia snapper_rocks spcr stadium stronghold subterra suburb surge tatooine tectonic tejen", 
  "tempest tortuga triforts tubes turbulence twinforts unworld3 urban_c valhalla warlock wdcd xenon"
}, " ")

ffamaps, capturemaps, ctfmaps = map.uv(function(maps)
  local t = map.f(I, maps:gmatch("[^ ]+"))
  for i = 2, #t do
    local j = math.random(i)
    local s = t[j]
    t[j] = t[i]
    t[i] = s
  end
  return table.concat(t, " ")
end, ffamaps, capturemaps, ctfmaps)

cs.maprotation("ffa effic tac teamplay efficteam tacteam", ffamaps, 
               "regencapture capture hold effichold instahold", capturemaps, 
               "ctf efficctf instactf protect efficprotect instaprotect", ctfmaps)
server.mastermask = server.MM_PUBSERV + server.MM_AUTOAPPROVE

local fp, L, ents = require"utils.fp", require"utils.lambda", require"std.ents"
local vec3 = require("utils.vec3");
local map = fp.map
local commands = require"std.commands"

local passes = {}
local function resetstreak(info) if not info.i then passes = {} else passes[info.i] = nil end end
spaghetti.addhook("changemap", resetstreak)
spaghetti.addhook("returnflag", resetstreak)
spaghetti.addhook("resetflag", resetstreak)

spaghetti.addhook("rugbypass", function(info)
  server.sendservmsg(server.colorname(info.actor, nil) .. " passed to " .. server.colorname(info.target, nil) .. "!")
  if server.m_hold or server.m_protect or info.actor.state.aitype ~= server.AI_NONE then return end   --sendresume with bots is problematic
  local i = next(info.flags)
  local streak = passes[i] or {}
  streak[info.actor.extra.uuid] = true
  passes[i] = streak
end)
spaghetti.addhook("scoreflag", function(info)
  local streak = passes[info.relay] or {}
  streak[info.ci.extra.uuid] = nil
  passes[info.relay] = nil
  streak = map.gp(uuid.find, streak)
  if #streak == 0 then return end
  for _, ci in ipairs(streak) do ci.state.flags = ci.state.flags + 1 server.sendresume(ci) end
  server.sendservmsg("Passes in flagrun (+1 flag point): " .. table.concat(map.li(L"server.colorname(_2, nil)", streak), ", "))
end)

local nextflagswitch = false
commands.add("flagswitch", function(info)
  local arg = info.args == "" and 1 or tonumber(info.args)
  if not arg then playermsg("Invalid flagswitch value", info.ci) end
  local old = nextflagswitch
  nextflagswitch = arg == 1
  if old == nextflagswitch then return end
  if nextflagswitch and (not server.m_ctf or server.m_hold) then playermsg("Mind that you still need to force the next mode to be ctf/protect.", info.ci) end
  server.sendservmsg(server.colorname(info.ci, nil) .. (nextflagswitch and " activated" or " deactivated") .. " \f1flag switch mode\f7 for the next map (see #help flagswitch).")
end, "Usage: #flagswitch [0|1]: activate flag switch (blue flag spawns in place of red and viceversa) for the next map if mode is ctf or protect (default 1, only masters)")

local flagswitch, currentflagswitch = require"gamemods.flagswitch", false
spaghetti.addhook("entsloaded", function()
  currentflagswitch = false
  nextflagswitch = nextflagswitch and server.m_ctf and not server.m_hold
  if not nextflagswitch then flagswitch.on(false) return end
  nextflagswitch = false
  flagswitch.on(true)
  currentflagswitch = true
end)

local ents = require"std.ents", require"std.maploaded"
require"std.pm"
require"std.getip"
require"std.specban"

--[[

require"std.discordrelay".new({
  relayHost = "127.0.0.1", 
  relayPort = 57575, 
  discordChannelID = "my-discord-channel-id",
  scoreboardChannelID = "my-scoreboard-channel-id",
  voice = {
    good = "good-voice-channel",
    evil = "evil-voice-channel"
  }
})

]]

spaghetti.addhook("entsloaded", function()
  if server.smapname ~= "thetowers" then return end
  for i, _, ment in ents.enum(server.JUMPPAD) do if ment.attr4 == 40 then
    ents.editent(i, server.JUMPPAD, ment.o, ment.attr1, ment.attr2, ment.attr3)
    break
  end end
end)

--moderation

cs.teamkillkick("*", 7, 30)

--limit reconnects when banned, or to avoid spawn wait time
abuse.reconnectspam(1/60, 5)

--limit some message types
spaghetti.addhook(server.N_KICK, function(info)
  if info.skip or info.ci.privilege > server.PRIV_MASTER then return end
  info.skip = true
  playermsg("No. Use gauth.", info.ci)
end)
spaghetti.addhook(server.N_SOUND, function(info)
  if info.skip or abuse.clientsound(info.sound) then return end
  info.skip = true
  playermsg("I know I used to do that but... whatever.", info.ci)
end)
abuse.ratelimit({ server.N_TEXT, server.N_SAYTEAM }, 0.5, 10, L"nil, 'I don\\'t like spam.'")
abuse.ratelimit(server.N_SWITCHNAME, 1/30, 4, L"nil, 'You\\'re a pain.'")
abuse.ratelimit(server.N_MAPVOTE, 1/10, 3, L"nil, 'That map sucks anyway.'")
abuse.ratelimit(server.N_SPECTATOR, 1/30, 5, L"_.ci.clientnum ~= _.spectator, 'Can\\'t even describe you.'") --self spec
abuse.ratelimit(server.N_MASTERMODE, 1/30, 5, L"_.ci.privilege == server.PRIV_NONE, 'Can\\'t even describe you.'")
abuse.ratelimit({ server.N_AUTHTRY, server.N_AUTHKICK }, 1/60, 4, L"nil, 'Are you really trying to bruteforce a 192 bits number? Kudos to you!'")
abuse.ratelimit(server.N_SERVCMD, 0.5, 10, L"nil, 'Yes I\\'m filtering this too.'")
abuse.ratelimit(server.N_JUMPPAD, 1, 10, L"nil, 'I know I used to do that but... whatever.'")
abuse.ratelimit(server.N_TELEPORT, 1, 10, L"nil, 'I know I used to do that but... whatever.'")

--prevent masters from annoying players
local tb = require"utils.tokenbucket"
local function bullying(who, victim)
  local t = who.extra.bullying or {}
  local rate = t[victim.extra.uuid] or tb(1/30, 6)
  t[victim.extra.uuid] = rate
  who.extra.bullying = t
  return not rate()
end
spaghetti.addhook(server.N_SETTEAM, function(info)
  if info.skip or info.who == info.sender or not info.wi or info.ci.privilege == server.PRIV_NONE then return end
  local team = engine.filtertext(info.text):sub(1, engine.MAXTEAMLEN)
  if #team == 0 or team == info.wi.team then return end
  if bullying(info.ci, info.wi) then
    info.skip = true
    playermsg("...", info.ci)
  end
end)
spaghetti.addhook(server.N_SPECTATOR, function(info)
  if info.skip or info.spectator == info.sender or not info.spinfo or info.ci.privilege == server.PRIV_NONE or info.val == (info.spinfo.state.state == engine.CS_SPECTATOR and 1 or 0) then return end
  if bullying(info.ci, info.spinfo) then
    info.skip = true
    playermsg("...", info.ci)
  end
end)

--ratelimit just gobbles the packet. Use the selector to add a tag to the exceeding message, and append another hook to send the message
local function warnspam(packet)
  if not packet.ratelimited or type(packet.ratelimited) ~= "string" then return end
  playermsg(packet.ratelimited, packet.ci)
end
map.nv(function(type) spaghetti.addhook(type, warnspam) end,
  server.N_TEXT, server.N_SAYTEAM, server.N_SWITCHNAME, server.N_MAPVOTE, server.N_SPECTATOR, server.N_MASTERMODE, server.N_AUTHTRY, server.N_AUTHKICK, server.N_CLIENTPING
)

--#cheater command
local home = os.getenv("HOME") or "."
local function ircnotify(args)
  --I use ii for the bots
  local cheaterchan, pisto = io.open(home .. "/irc/cheaterchan/in", "w"), io.open(home .. "/irc/ii/pipes/pisto/in", "w")
  for ip, requests in pairs(args) do
    local str = "#cheater" .. (requests.ai and " \x02through bots\x02" or "") .. " on pisto.horse 1024"
    if requests.total > 1 then str = str .. " (" .. requests.total .. " reports)" end
    str = str .. ": "
    local names
    for cheater in pairs(requests.cheaters) do str, names = str .. (names and ", \x02" or "\x02") .. engine.encodeutf8(cheater.name) .. " (" .. cheater.clientnum .. ")\x02", true end
    if not names then str = str .. "<disconnected>" end
    if cheaterchan then cheaterchan:write(str .. ", auth holders please help!\n") end
    if pisto then pisto:write(str .. " -- " .. tostring(require"utils.ip".ip(ip)) .. "\n") end
  end
  if cheaterchan then cheaterchan:write('\n') cheaterchan:close() end
  if pisto then pisto:write('\n') pisto:close() end
end

-- deactivate this for the discord bot
--abuse.cheatercmd(ircnotify, 20000, 1/30000, 3)

local sound = require"std.sound"
spaghetti.addhook(server.N_TEXT, function(info)
  if info.skip then return end
  local low = info.text:lower()
  if not low:match"cheat" and not low:match"hack" and not low:match"auth" and not low:match"kick" then return end
  local tellcheatcmd = info.ci.extra.tellcheatcmd or tb(1/30000, 1)
  info.ci.extra.tellcheatcmd = tellcheatcmd
  if not tellcheatcmd() then return end
  playermsg("\f2Problems with a cheater? Please use \f3#cheater [cn|name]\f2, and operators will look into the situation!", info.ci)
  sound(info.ci, server.S_HIT, true) sound(info.ci, server.S_HIT, true)
end)

require"std.enetping"

local parsepacket = require"std.parsepacket"
spaghetti.addhook("martian", function(info)
  if info.skip or info.type ~= server.N_TEXT or info.ci.connected or parsepacket(info) then return end
  local text = engine.filtertext(info.text, true, true)
  engine.writelog(("limbotext: (%d) %s"):format(info.ci.clientnum, text))
  info.skip = true
end, true)

--simple banner

local git = io.popen("echo `git rev-parse --short HEAD` `git show -s --format=%ci`")
local gitversion = git:read()
git = nil, git:close()
commands.add("info", function(info)
  playermsg("spaghettimod is a reboot of hopmod for programmers. Will be used for SDoS.\nKindly brought to you by pisto." .. (gitversion and "\nCommit " .. gitversion or ""), info.ci)
end)

local function gamemoddesc()
  local msg
  if ents.active() and currentflagswitch then msg = "\n\f1Flag switch mode activated\f7! " .. (server.m_protect and "Your flag spawns in the enemy base!" or "Bring the enemy flag back to the enemy base!") end
  if server.m_ctf then msg = (msg or "") .. "\n\f3Rugby mode activated\f7! Shoot a teammate to pass the flag you are carrying" end
  return msg
end

spaghetti.later(20000, function()
  return server.m_ctf and server.sendservmsg("\nRemember, it's Rugby mode: you \f6shoot a \f0nearby \f6teammate \f7with rifle to \f6pass the flag\f7!")
end, true)

--lazy fix all bugs.

spaghetti.addhook("noclients", function()
  if engine.totalmillis >= 24 * 60 * 60 * 1000 then reboot, spaghetti.quit = true, true end
end)
