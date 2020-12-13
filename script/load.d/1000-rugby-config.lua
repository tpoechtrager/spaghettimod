--[[

  This is a dedicated rugby configuration.

]]--

if not os.getenv("RUGBY") then return end
engine.writelog("Applying the rugby configuration.")

local servertag = require"utils.servertag"
servertag.tag = "rugby"

local uuid = require"std.uuid"

local fp, L = require"utils.fp", require"utils.lambda"
local map, I = fp.map, fp.I
local abuse, playermsg = require"std.abuse", require"std.playermsg"

cs.maxclients = 20
cs.serverport = 28785

cs.updatemaster = 1
spaghetti.later(10000, L'engine.requestmaster("\\n")', true)
spaghetti.addhook("masterin", L'if _.input:match("^failreg") then engine.lastupdatemaster = 0 end', true)

cs.autorecorddemo = 1

--add here your own auth configuration.
local auth = require"std.auth"
cs.serverauth = "spaghetti"
table.insert(auth.preauths, "spaghetti")

cs.adduser("benzomatic", "spaghetti", "+a26e607b5554fd5b316a4bdd1bfc4734587aa82480fb081f", "a")

cs.publicserver = 1

spaghetti.addhook(server.N_SETMASTER, L"_.skip = _.skip or (_.mn ~= _.ci.clientnum and _.ci.privilege < server.PRIV_AUTH)")

cs.serverdesc = ":: Rugby Server ::"

cs.lockmaprotation = 2
cs.ctftkpenalty = 0
cs.maprotationreset()

--copied from data/menus.cfg
local ctfmaps = table.concat({
  "aastha abbey akimiski akroseum arbana asgard authentic autumn bad_moon berlin_wall bklyn breakout bt_falls campo capture_night casa",
  "catch22 collide core_refuge core_transfer croma ctf_suite daemex damnation desecration destiny disc disruption divine donya duomo dust2",
  "earthsea earthstation enigma eris eternal_valley europium evilness face-capture fc4 fc5 fire_keep flagstone forge forgotten fortress", 
  "fragnostic fusion garden gubo hallo harbor haste hidden idris infamy kiryu kopenhagen l_ctf laucin luna mach2 mbt1 mbt4 mbt12 mc-lab", 
  "meltdown2 mercury metro mill new_energy nitro nucleus overdrive ow pandora pul1ctf ra recovery redemption regal reissen risk river_keep",
  "ruebli rust sacrifice shellshock2 shipwreck siberia snapper_rocks spcr stadium stronghold subterra suburb surge tatooine tectonic tejen", 
  "tempest tortuga triforts tubes turbulence twinforts unworld3 urban_c valhalla warlock wdcd xenon"
}, " ")

ctfmaps = map.uv(function(maps)
  local t = map.f(I, maps:gmatch("[^ ]+"))
  for i = 2, #t do
    local j = math.random(i)
    local s = t[j]
    t[j] = t[i]
    t[i] = s
  end
  return table.concat(t, " ")
end, ctfmaps)

cs.maprotation("instactf", ctfmaps)

local fp, L, ents = require"utils.fp", require"utils.lambda", require"std.ents"
local vec3 = require("utils.vec3");
local map = fp.map
local commands = require"std.commands"


-- Rugby configuration

-- Standard rugby (unlimited passing range)
spaghetti.addhook("changemap", L'require"gamemods.rugby".on(server.m_ctf)')

-- Rugby, but you can only pass after a specified time
--spaghetti.addhook("changemap", L'require"gamemods.delayedrugby".on(server.m_ctf)')

-- Rugby, but you can only pass backwards (like in actual rugby)
--spaghetti.addhook("changemap", L'require"gamemods.realrugby".on(server.m_ctf)')

-- Rugby, but you can pass only a certain distance relative to map size. 
-- Players that can be passed to are highlighted.
--spaghetti.addhook("changemap", L'require"gamemods.highlightrugby".on(server.m_ctf)')


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

spaghetti.later(30000, function()
  if not server.m_ctf then return end
  server.sendservmsg("\n\f3Rugby mode activated\f7! Shoot a teammate to pass the flag you are carrying")
end, true)

--lazy fix all bugs.

spaghetti.addhook("noclients", function()
  if engine.totalmillis >= 24 * 60 * 60 * 1000 then reboot, spaghetti.quit = true, true end
end)
