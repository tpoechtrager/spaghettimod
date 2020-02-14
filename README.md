
# spaghettimod-extra
A collection of additional scripts I created for [spaghettimod](https://github.com/pisto/spaghettimod). 

Short descriptions can be found below. 

## Discord bot

Monitor and control servers through discord: 
An application consisting of a NodeJS discord bot and a LUA module, communicating through UDP to send and receive gameserver messages and discord commands. 
You need a discord bot that is invited to your server, and its API token, both of which  can be created [here](https://discordapp.com/developers/applications/).

**Required bot permissions:** 
Additionally to the standard Send/Read Messages, make sure to have Manage Messages and Read Message History enabled. Usually granted automatically (yet important) are permissions to send embeds and attach files (for thumbnails), and the ability to issue @here mentions if alerts are enabled.
If auto-voice is used, it also needs Move Members permissions in the given voice channels.

### Setup

#### 1) Set up NodeJS, npm and dependencies

* Install NodeJS and npm
* Drop the `discord` folder into the spaghettimod directory
* `cd discord`
* `npm install` to fetch the required modules

#### 2) Set up the LUA module
* Drop this repo's `script/std/discordrelay.lua` into your `script/std` folder

### Configuration

#### 1) NodeJS: Open discord/config.json and configure the following:
* `discordToken` is your discord API bot token
* `commandPrefix` is the bot command prefix
* `alerts` will activate #cheater alerts
* `alertsChannel` is an optional dedicated channelID for #cheater alerts
* `useEmbeds` to display the server broadcast in embeds; if disabled, will use classic text messages in the main channel
* `enableThumbnails` will show a fancy map preview on status messages
* `enableScoreboard` will show a scoreboard with every player's stats appended to a status message
* `alwaysScoreboard` will show a scoreboard on every status message if there is no seperate scoreboard channel
* `relayHost`  should stay 127.0.0.1
* `relayPort` can be any free port, but the port in the LUA config must be the same


#### 2) LUA: Open the main config of your server (1000-sample-config.lua in my case) and add the following:

```
require"std.discordrelay".new({
  relayHost = "127.0.0.1", 
  relayPort = 57575, 
  discordChannelID = "my-discord-channel-id",
  scoreboardChannelID = "my-scoreboard-channel-id",
  voice = {
    good = "good-channel-ID",
    evil = "evil-channel-ID"
  }
})
```

* where `relayHost`  and `relayPort` **must** match what is configured in NodeJS and
* `discordChannelID` is the channel ID that this server will send messages to, and commands will be read from
* `scoreboardChannelID` is an optional channel for a dedicated scoreboard that will update by the minute
* `voice` is an optional table that maps team names to voice channels in discord. People that link themselves via #voice will be auto-synchronised with the respective team channel

#### #voice

If voice channels were linked, a new command `#voice` will be added. Users can:
* join a voice channel and type `#voice`: The bot will link the client if it finds a similar name in the voice channels
* type `#voice pw`: The user will be sent a code to pm to the discord bot; the user will log in after that
* type `#voice <code>`: The user provides a code that the bot had sent to them earlier, and they will log in as well.

### Launching the bot

#### 1) cd into `discord` and start the discord bot like this or through a process manager:

```
cd discord
node app
```

#### 2) after that, start spaghettimod: 

Make sure to disable the #cheater command functionality in your main config, as the discord module activates a new version of #cheater. You might get a duplicate error otherwise.
```
YOURENV=true ./sauer_server
```
The NodeJS bot supports different unique servertags, so if you have multiple gameservers, you can repeat the **Step 2) LUA** configuration for every server and monitor them in their respective channels.

If everything went well, the bot should be marked online and a success message will appear in the provided discord channel.

## Rugby modifications

**script/gamemods/delayedrugby.lua** 

*Rugby mode, but with a minimum time that has to pass before the flag carrier can pass to teammates.*

**script/gamemods/limitedrugby.lua**

*Rugby mode, but only backwards passes are allowed.*

**script/gamemods/rangedrugby.lua**

*Rugby mode, but with a pass range restriction to all sides. Players that can be passed to are highlighted with overhead particles.*

## Miscellaneous

**script/std/autobalance.lua**

*Automatic balancing based on the team player counts. Uses soft and hard balance to even out the teams.*

**script/std/easter.lua**

*Activate easter mode with custom easter eggs that grant special effects!*

**script/std/pastanames.lua**

*Automatically give unnamed players a random but delicious pasta name on connect.*

**script/std/specban.lua**

*Implement IP-based specbanning, featuring optional time durations, reasons and a specbanlist.*

**script/std/vpnban.lua**

*Ban non-residential IPs and proxies from connecting using the free [iphub.info](https://iphub.info) API.*
