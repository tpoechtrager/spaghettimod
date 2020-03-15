/***********************************************************************
    This NodeJS discord bot exchanges spaghettimod event messages 
    and discord commands. Launch this before launching spaghettimod.
***********************************************************************/

const fs = require('fs'),
      dgram = require('dgram'),
      Discord = require('discord.js');

const server = dgram.createSocket('udp4'),
      client = new Discord.Client();

const { bot, udp } = require('./config.json'),
      countries = require('./countries.json');

let { discordToken, commandPrefix: prefix, alerts, alertChannelID, useEmbeds, enableThumbnails, enableScoreboard, alwaysScoreboard } = bot,
    { relayHost, relayPort } = udp;


/***
    Utils
***/

function log(s) {
  let d = new Date();
  d = d.toUTCString();
  return console.log(`[${d}] ${s}`);
}

function out(e, errorflag) {
  if (!errorflag) errorflag = 'Discord';
  if (e) return log(`[${errorflag} Error] ${e}`);
}

function addspaces(n) { // force a minimum width on embeds
  let str = '';
  for (let i = 0; i < n; i++) str += '\u0020';
  return str;
}

let ipregex = new RegExp([
    /\b(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])\./,
    /(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])\./,
    /(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])\./,
    /(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])/,
    /(\/[0-2]\d|\/3[0-2])?/
  ].map(r => r.source).join(''));

let ipregexgm = new RegExp(ipregex.source, 'gm');

function maskIPs(str) { // anonymize IPs but still leave room to differentiate between them
  let ipmasked = false;
  str = str.replace(ipregexgm, n => {
    let m = ipregex.exec(n);
    if (m) {
      let mask = (m[5] !== undefined) ? m[5] : "";
      m = m.map(item => ((item !== undefined) && item.padStart(2, 'x').padEnd(3, 'x').replace(/^.{1}/g, 'x').replace(/.$/, 'x'))).splice(1, 4).join(".") + mask,
      ipmasked = true;
    }
    return m;
  });
  return (ipmasked) ? str + "_ -> IPs were masked. Please join the server to retrieve IPs._" : str;
}


/***
    Create the UDP instance that will listen to gameserver messages to be relayed to discord.
***/

let servers = {},
    channels = {},
    statusIDs = {},
    voice = {};

function updateServer(tag, channel, rinfo) {
  if (!servers[tag]) {
    servers[tag] = { channel: channel, address: rinfo.address, port: rinfo.port };
  } else {
    servers[tag].channel = channel, servers[tag].address = rinfo.address, servers[tag].port = rinfo.port;
  }
  
  if (!channels[channel]) {
    channels[channel] = {address: rinfo.address, port: rinfo.port};
  } else { 
    channels[channel].address = rinfo.address, channels[channel].port = rinfo.port;
  }
}

function sendUDP(tbl, address, port) {
  let str = JSON.stringify(tbl);
  let buf = new Buffer.from(str);
  return server.send(buf, 0, buf.length, port, address, (e) => out(e, 'UDP'));
}

let perms = [ 'SEND_MESSAGES', 'VIEW_CHANNEL', 'EMBED_LINKS', 'READ_MESSAGE_HISTORY', 'MANAGE_MESSAGES' ],
    vperms = [ 'VIEW_CHANNEL', 'MOVE_MEMBERS' ];

function missingAccess(channel) {
  if (alerts && !perms.includes('MENTION_EVERYONE')) perms.push('MENTION_EVERYONE');
  if (enableThumbnails && !perms.includes('ATTACH_FILES')) perms.push('ATTACH_FILES');
  let needed = [];
  perms.forEach((flag) => { if(!channel.permissionsFor(client.user).has(flag)) needed.push(flag) });
  return (needed.length ? needed.join(' and ') : false);
}

function missingVoiceAccess(channel) {
  let vneeded = [];
  vperms.forEach((flag) => { if(!channel.permissionsFor(client.user).has(flag)) vneeded.push(flag) });
  return (vneeded.length ? vneeded.join(' and ') : false);
}

function canPurge(channel) {
  return channel.permissionsFor(client.user).has('READ_MESSAGE_HISTORY', 'MANAGE_MESSAGES');
}

function purge(channelID, logpurge) {
  return new Promise((resolve, reject) => {
    let channel = client.channels.resolve(channelID);
    if (!channel) return reject('Purge Error: Channel not found.');
    if (!canPurge(channel))
      return reject(`Purge Error: I need Read Message History and Manage Messages permission in #${channel.name}. I need those to clean up the channel.`);
    channel.messages.fetch({ limit: 10 })
    .then((collected) => collected.filter(msg => msg.author.id === client.user.id))
    .then((botMessages) => channel.bulkDelete(botMessages))
    .then((botMessages) => {
      if (botMessages.size)
        if (logpurge) log(`[Discord] Purged ${botMessages.size} old status messages from #` + channel.name);
        return resolve();
    })
    .catch((e) => reject(e));
  })
}

function pushStatus(tag, channel, important, scoreboardChannelID, embed, scoreboard, forcepurge) {
  let scoreboardChannel = client.channels.resolve(scoreboardChannelID);
  if (!scoreboardChannel) return;
  
  forcepurge = (forcepurge || (servers[tag].usingEmbeds !== useEmbeds));
  if (!forcepurge && statusIDs[scoreboardChannelID] && statusIDs[scoreboardChannelID].embed && statusIDs[scoreboardChannelID].scoreboard) {
    let oldembed = scoreboardChannel.messages.resolve(statusIDs[scoreboardChannelID].embed);
    oldembed.edit(embed).catch((e) => out(e, 'Scoreboard Edit'));
    scoreboardChannel.messages.resolve(statusIDs[scoreboardChannelID].scoreboard)
    .edit(scoreboard)
    .catch((e) => out(e, 'Scoreboard Edit'));
    if (important) channel.send(embed).catch((e) => out(e, 'Scoreboard Send'));
   return;
  }

  servers[tag].usingEmbeds = useEmbeds;
  let embedid = '';
  if (statusIDs[scoreboardChannelID]) {
    delete statusIDs[scoreboardChannelID].embed;
    delete statusIDs[scoreboardChannelID].scoreboard;
  }
  purge(scoreboardChannelID)
  .then(() =>  scoreboardChannel.send(embed).catch((e) => out(e)))
  .then((embedmsg) => embedid = embedmsg.id)
  .then(() => scoreboardChannel.send(scoreboard))
  .then((sbmsg) => statusIDs[scoreboardChannelID] = { embed: embedid, scoreboard: sbmsg.id })
  .catch((e) => out(e, 'Scoreboard Send'));
  if (important) channel.send(embed).catch((e) => out(e, 'Scoreboard Send'));
  return;
}

function synchronizeVoice(userid, vchannelid) {
  return new Promise((resolve, reject) => {
    if (!vchannelid) return reject('Voice channel not found.');
    let vchannel = client.channels.resolve(vchannelid)
    if (!vchannel) return reject('Voice channel not found.');
    if (missingVoiceAccess(vchannel)) return reject(`Lacking ${missingVoiceAccess(vchannel)} permissions in ${vchannel.name}`);
    let guild = vchannel.guild;
    if (!guild) return reject('Guild not found.');
    if (!client.users.cache.get(userid)) return reject(`Discord user for '${userid}' not found!`);
    guild.members.fetch(userid)
    .then((member) => {
      if(!member.voice.channelID || (member.voice.channelID == vchannel.id)) return resolve();
      let chans = [];
      for (const [tag] of Object.entries(servers)) {
        for (const [chan, info] of Object.entries(voice[tag].channels)) {
          if (chan != "names") chans.push(info.id);
        }
      }
      if (chans.includes(member.voice.channelID)) {
        member.voice.setChannel(vchannelid)
        .then(() => resolve())
        .catch((e) => reject(e));
      }
    })
    .catch((e) => reject(e));
  });
}

function levenshtein(a, b) { // rosettacode.org
  var t = [], u, i, j, m = a.length, n = b.length;
  if (!m) { return n; }
  if (!n) { return m; }
  for (j = 0; j <= n; j++) { t[j] = j; }
  for (i = 1; i <= m; i++) {
    for (u = [i], j = 1; j <= n; j++) {
      u[j] = a[i - 1] === b[j - 1] ? t[j - 1] : Math.min(t[j - 1], t[j], u[j - 1]) + 1;
    } t = u;
  } return u[n];
}

function collectUsers(guild, voiceUsers, list, comp) {
  return new Promise((resolve, reject) => {
    let tmp = voiceUsers;
    if (!voiceUsers.size) return resolve();
    voiceUsers.forEach((_, id) => {
      guild.members.fetch(id)
      .then((user) => {
        let exists = false;
        for (const [tag, info] of Object.entries(voice)) {
          for (const [id, obj] of Object.entries(voice[tag].ids)) {
            if (obj.discordid && obj.discordid == user.id) exists = true;
          }
        }
        if (!exists && ((levenshtein(comp, user.displayName) / comp.length) <= 9/20)) 
          list[id] = { name: user.displayName, channelid: user.voice.channelID }; 
        delete tmp[id];
        if (!tmp.length) return resolve();
      })
      .catch((e) => reject(e));
    });
  });
}

server.on('error', (e) => {
  out(e, 'UDP');
  server.close();
});

server.on('listening', () => log(`[Discord] Interface listening for messages on ${relayHost}:${relayPort}`));

server.on('message', (msg, rinfo) => {
  
  let full = JSON.parse(msg);
  let requestesc = JSON.stringify(full).replace(/@(everyone|here)/g, '@\u200b$1');
  let request = JSON.parse(requestesc);

  let args = request.msg;
  let event = request.msg.event;

  let oldrinfo;
  if (servers[request.tag]) oldrinfo = servers[request.tag].port;
  updateServer(request.tag, request.channel, rinfo);

  const channel = client.channels.resolve(servers[request.tag].channel);
  if (!channel) return;
  if (missingAccess(channel))
    return out(`I need ${missingAccess(channel)} permissions in #${channel.name}!`, 'Permissions');

  let hasScoreboardChannel = false;
  const scoreboardChannelID = request.scoreboardChannel;
  const scoreboardChannel = client.channels.resolve(scoreboardChannelID);
  if (scoreboardChannel && (scoreboardChannel != '')) {
    if (!missingAccess(scoreboardChannel)) {
      hasScoreboardChannel = true;
    } else {
      out(`I need ${missingAccess(scoreboardChannel)} permissions in #${scoreboardChannel.name}!`, 'Permissions');
    }
  }

  let hasVoice = false;
  const voiceChannels = request.voice;
  if (!voice[request.tag]) voice[request.tag] = {};
  if (voiceChannels && (!voice[request.tag].channels || (oldrinfo != rinfo.port))) { 
    voice[request.tag].channels = {};
    voice[request.tag].requests = {};
    voice[request.tag].ids = {};
    let lastguildid,
        msg = "",
        verror = false;
    for (const [team, channelid] of Object.entries(voiceChannels)) {
      let tmpchan = client.channels.resolve(channelid);
      if (!tmpchan) msg = "Channel not found";  
      if (tmpchan) {
        for (const [tag] of Object.entries(servers)) { 
          if (voice[tag].channels[team] && (voice[tag].channels[team].id == tmpchan.id))
            msg = `The channel already exists in '${tag}'`;
        }
        if (!lastguildid) lastguildid = tmpchan.guild.id;
        if ((msg == "") && tmpchan.guild.id != lastguildid) msg = "The channel is in a different guild";
        if ((msg == "") && (tmpchan.type !== 'voice')) msg = "The channel is not a voice channel";
        if ((msg == "") && missingVoiceAccess(tmpchan)) msg = `Lacking ${missingVoiceAccess(tmpchan)} permissions in ${tmpchan.name}`;
      }
      if (msg != "") {
        log(`[Voice Error] Could not add voice channel for team '${team}' @${request.tag}: ${msg}!`);
        msg = "";
        verror = true;
      } else if (!verror){
        hasVoice = true;
        if (!voice[request.tag].channels[team]) voice[request.tag].channels[team] = {};
        if (!voice[request.tag].channels.names) voice[request.tag].channels.names = [];
        voice[request.tag].channels[team].id = tmpchan.id;
        voice[request.tag].channels[team].guildid = tmpchan.id;
        voice[request.tag].channels.names.push(`:loud_sound:**${tmpchan.name}**`);
        log(`[Voice] Added a voice channel: Team '${team}' @${request.tag} <-> Channel '${tmpchan.name}' @${tmpchan.guild.name}`);
      }
    }
    if ((msg != "") || verror) {
      hasVoice = false;
      log(`[Voice] Voice functionality for '${request.tag}' disabled completely.`);
    }
  } else if (voiceChannels && voice[request.tag].channels) hasVoice = true;

  let flag = ((args.country && (args.country != 'Unknown') && countries[args.country]) ? ':flag_' + countries[args.country] + ':' : '[ ? ]'),
      cmsg = '', color = 0x989898,
      author = '', authorname = '', preauthor = '',
      title = '', description = '', thumbnail = '',
      fields = [], timestamp = '', footer = '' ,
      scoreboard = '', map = '',
      isCommand = false, isStatus = false, isImportant = false, isNewgame = false;
   
  switch (event) {

    case 'register':
      if (servers[request.tag]) {
        servers[request.tag].name = args.server;
        servers[request.tag].usingEmbeds = useEmbeds;
      }
      cmsg = `---\nI'm here now! This channel is linked to \'${args.server}'.\n`;
      cmsg += `Type **${prefix}help** for this server's command list.`;
      if (enableScoreboard && hasScoreboardChannel) {
        if (statusIDs[scoreboardChannelID]) delete statusIDs[scoreboardChannelID];
        purge(scoreboardChannelID, true).catch((e) => out(e, 'Scoreboard Purge'));
        cmsg += `\nA scoreboard will appear in <#${scoreboardChannel.id}> once a map is switched or a game is running.`;
      }
      cmsg += '\n---';
      let tbl = {
        cmd: '#regsuccess',
        args: {}
      };
      sendUDP(tbl, rinfo.address, rinfo.port);
      if (hasVoice) {
        let vtbl = {
          voice: true,
          cmd: '#voiceregsuccess',
          guildname: channel.guild.name,
          botname: getNick(client.user, channel.guild)
        };
        sendUDP(vtbl, rinfo.address, rinfo.port);
      }
      log(`[Discord] Registered server '${args.server}' ['${request.tag}'] @${rinfo.address}:${rinfo.port}`);
      break;

    // Command echos

    case 'cmderr':
      cmsg = `<@${args.userid}>\n${args.msg}`;
      break;

    case 'cmdhelp':
      authorname = 'COMMANDS';
      description = `**List of bot commands:**\n\n **.embeds:**\nToggle whether or not embeds are shown.\n\n\u200b`;
      description += `**List of commands for '${args.server}':**\n_Usage: ${prefix}<command> <args> [<opt. args>]_\n\u200b`;
      color = 0x6485ff;
      if (!useEmbeds) cmsg += description + '\n';
      for (let cmd in args.list) {
        if (args.list.hasOwnProperty(cmd)) {
          if (!useEmbeds) {
            cmsg += `**${prefix}${args.list[cmd].name} ${args.list[cmd].cmdargs}**\n${args.list[cmd].help}\n`;
          } else {
            fields.push({ name: prefix + args.list[cmd].name + ' ' + args.list[cmd].cmdargs, value: args.list[cmd].help });
          }
        }
      }
      break;

    case 'cmdbanlist':
      authorname = `BANLIST ${addspaces(140)}\u200b`;
      let choice = (args.all ? `by banlist` : `in banlist '${args.blist.name}'`)
      description = `Enumerating bans ${choice}:\nExpire :: Range :: Reason`;
      color = 0x6485ff;
      if (!useEmbeds) cmsg += '**' + description + '**\n';
      if (args.all) {
        if (!useEmbeds) cmsg += '```';
        for (let banlist in args.lists) {
          if (args.lists.hasOwnProperty(banlist)) {
            if (!useEmbeds) {
              cmsg += `${args.lists[banlist].name}:\n${args.lists[banlist].list}\n\n`;
            } else {
              fields.push({ name: args.lists[banlist].name, value: '```\n' + args.lists[banlist].list + '\n```' });
            }
          }
        }
        if (!useEmbeds) cmsg += '```';
      } else {
        if (!useEmbeds) {
          cmsg += '```\n' + args.blist.list + '\n```';
        } else {
          description += '```\n' + args.blist.list + '\n```'
        }
      }
      break;

    case 'cmdstatus':
      authorname = 'SERVER STATUS';
      description = `Mode/map: **${args.modemap}**\nPlayers: **${args.players}**\n\n`;
      scoreboard = '```diff\nServing ' + args.players + ' (high frags/acc/tks are highlighted):\n\n' + args.str + '\n```';
      if (!useEmbeds) {
        cmsg += ':information_source: SERVER STATUS: ' + description.replace(/\n/g, '\u0020');
        cmsg += scoreboard;
      }
      map = args.nmap;
      isCommand = true;
      color = 0x2aa198;
      timestamp = new Date();
      break;

    case 'cmdsay':
      cmsg = `${args.name}: [REMOTE] ${args.txt}`;
      if (!useEmbeds) cmsg = `:speech_balloon: **${args.name}**: [REMOTE] ${args.txt}`;
      break;

    case 'cmdstats':
      authorname = `STATS ${addspaces(100)}\u200b`;
      description = `**Name:** ${args.name} (${args.cn})\n`;
      description += `**Location**: ${args.location} ${flag}\n`;
      description += `**Playtime**: ${args.contime}\n\n`;
      description += '```diff\nStats: ' + args.stats + '\n```';
      if (!useEmbeds) {
        cmsg = `:information_source: STATS for ${args.name} (${args.cn}) (${flag} ${args.country}):\n`;
        cmsg += '```diff\n'
        cmsg += `Name: ${args.name} (${args.cn})\n`;
        cmsg += `Location: ${args.location}\n`;
        cmsg += `Playtime: ${args.contime}\n\n`;
        cmsg += 'Stats: ' + args.stats + '\n```';
      }
      color = 0xffa700;
      break;

    case 'cmdgetip':
      authorname = 'IP';
      description = `**Name:** ${args.name} (${args.cn})\n`;
      description += `**Location**: ${args.location} ${flag}\n`;
      description += `**Playtime**: ${args.contime}\n\n`;
      description += `**IP**: ${args.ip}\n\n`;
      if (!useEmbeds) {
        cmsg = `:information_source: IP for ${args.name} (${args.cn}) (${flag} ${args.country}):\n`;
        cmsg += '```diff\n'
        cmsg += `Name: ${args.name} (${args.cn})\n`;
        cmsg += `Location: ${args.location}\n`;
        cmsg += `Playtime: ${args.contime}\n\n`;
        cmsg += 'IP: ' + args.ip + '\n```';
      }
      color = 0xffa700;
      break;

    // Server broadcast

    case 'chat':
      cmsg = `${args.name} (${args.clientnum}): ${args.txt}`;
      if (!useEmbeds) cmsg = `:speech_balloon: **${args.name} (${args.clientnum})**: ${args.txt}`;
      break;

    case 'info':
      description = args.txt;
      if (!useEmbeds) cmsg = `:information_source: INFO: ${args.txt}`;
      break;

    case 'rename':
      authorname = 'RENAME';
      description = `${args.oldname} (${args.clientnum}) is now known as **${args.newname}**`;
      if (!useEmbeds) cmsg = `:arrows_counterclockwise: RENAME: **${args.oldname} (${args.clientnum})** is now known as ${args.newname}`;
      color = 0x3a56bd;
      break;

    case 'connected':
      authorname = 'CONNECT';
      description = `${args.name} (${args.clientnum}) connected from ${flag} ${args.country}`;
      if (!useEmbeds) cmsg = `:arrow_right: CONNECT: **${args.name} (${args.clientnum})** connected from ${flag} ${args.country}`;
      color = 0x6485ff;
      break;

    case 'disconnected':
      authorname = 'DISCONNECT';
      description = `${args.name} (${args.clientnum}) disconnected after ${args.contime}${args.reason}`;
      if (!useEmbeds) {
        let prefix = (args.iskick ? ':put_litter_in_its_place:' : ':arrow_left:')
        cmsg = `${prefix} DISCONNECT: **${args.name} (${args.clientnum})** disconnected after ${args.contime}${args.reason}`;
      }
      color = (args.iskick ? 0xff0000 : 0x0036f9);
      break;

    case 'master':
      authorname = 'MASTER';
      description = `${args.name} (${args.clientnum}) ${args.action}`;
      if (!useEmbeds) cmsg = `:passport_control: MASTER: **${args.name} (${args.clientnum})** ${args.action}`;
      break;

    case 'kick':
      authorname = 'KICKBAN';
      description = args.str;
      if (!useEmbeds) cmsg = `:x: KICKBAN: **${args.str}**`;
      color = 0xff0000;
      break;

    case 'newgame':
      preauthor = ':new: ';
      authorname += 'NEW GAME';
      description = `Mode/map: **${args.modemap}**\nPlayers: **${args.players}**\n\n`;
      if (!useEmbeds) cmsg = ':new: NEW GAME: ' + description.replace(/\n/g, '\u0020');
      scoreboard = '```diff\nListing ' + args.players + ' (high acc/tks are highlighted):\n\n' + args.str + '\n```';
      map = args.nmap;
      isStatus = true;
      isImportant = true;
      isNewgame = true;
      color = 0x0aff01;
      footer.text = 'Last update';
      timestamp = new Date();
      break;

    case 'status':
      // only display every second status update in the light version
      preauthor = ':information_source: ';
      authorname += `SERVER STATUS ${args.progress}`;
      description = `Mode/map: **${args.modemap}**\nPlayers: **${args.players}**\n\n`;
      if (!useEmbeds && (Math.abs(args.n % 2) == 0)) 
        cmsg = ':information_source: SERVER STATUS (' + (args.n / 2) + '/5): ' + description.replace(/\n/g, '\u0020');
      if (useEmbeds) description += '_Updates every minute._';
      scoreboard = '```diff\nListing ' + args.players + ' (high acc/tks are highlighted):\n\n' + args.str + '\n```';
      map = args.nmap;
      isStatus = true;
      color = 0x2aa198;
      footer.text = 'Last update';
      timestamp = new Date();
      break;

    case 'intermission':
      preauthor = ':stop_button: ';
      authorname += 'INTERMISSION';
      description = `Mode/map: **${args.modemap}**\nPlayers: **${args.players}**\n\n`;
      if (!useEmbeds) cmsg = ':stop_button: INTERMISSION: ' + description.replace(/\n/g, '\u0020');
      scoreboard = '```diff\nListing ' + args.players + ' (high acc/tks are highlighted):\n\n' + args.str + '\n```';
      map = args.nmap;
      isStatus = true;
      isImportant = true;
      color = 0xffa700;
      footer.text = 'Last update';
      timestamp = new Date();
      break;

    // #cheater

    case 'alert':
      if (!alerts) break;
      let alertmsg = maskIPs('\u200b\n```diff\n' + args.subject + '\n```\n_reported by ' + args.reporters + '_\n\nPlease join the server to evaluate.\n\u200b');
      const alert = new Discord.MessageEmbed()
        .setColor(0xffa700) 
        .setAuthor(`CHEATER REPORT: '${args.server}'${addspaces(70)}\u200b`) 
        .setDescription(alertmsg) 
        .setTimestamp(new Date());
      if (alertChannelID && (alertChannelID != '')) {
        const achannel = client.channels.resolve(alertChannelID);
        if (achannel) {
          if (missingAccess(achannel))
            return out(`I need ${missingAccess(achannel)} permissions in #${achannel.name}!`, 'Permissions');
          if (useEmbeds) { 
            achannel.send('@here', alert).catch((e) => out(e, 'Send'));
          } else {
            achannel.send('@here' + alertmsg).catch((e) => out(e, 'Send'));
          }
        } else {
          if (useEmbeds) { 
            channel.send('@here', alert).catch((e) => out(e, 'Send'));
          } else {
            channel.send('@here' + alertmsg).catch((e) => out(e, 'Send'));
          }
        }
      } else {
        if (useEmbeds) { 
          channel.send('@here', alert).catch((e) => out(e, 'Send'));
        } else {
          channel.send('@here' + alertmsg).catch((e) => out(e, 'Send'));
        }
      }
      break;

    case 'voiceevent':
      if (!hasVoice) return;
      let action = args.action;

      switch(action) {

        case 'getsimilar':
          voice[request.tag].ids[args.id] = { tag: request.tag, cn: args.cn, team: args.team, sname: args.sname, guild: channel.guild.name };
          let voiceUsers = channel.guild.members.cache.filter(member => !!member.voice.channelID),
              list = {};
          let fail = false;
          let _tbl = {
            voice: true,
            cmd: '#similarfail',
            guildname: channel.guild.name,
            botname: getNick(client.user, channel.guild),
            cn: args.cn
          };
          collectUsers(channel.guild, voiceUsers, list, args.name)
          .then(() => {  
            if (!Object.keys(list).length) {
              _tbl.reason = "no matching name was found in any voice channel", _tbl.rec = "join voice";
              return sendUDP(_tbl, rinfo.address, rinfo.port);
            }
            let chans = [];
            for (const [chan, info] of Object.entries(voice[request.tag].channels)) {
              if (chan != "names") chans.push(info.id);
            }
            let userid = Object.keys(list)[0];
            let username = list[userid].name,
                channelid = list[userid].channelid;
            if (!chans.includes(channelid)) _tbl.reason = "you are not in an active voice channel", _tbl.rec = "join active channels", fail = true; 
            if (Object.keys(list).length > 1) _tbl.reason = "multiple users were found", _tbl.rec = "rename", fail = true;
            if (!voice[request.tag].channels[args.team]) _tbl.reason = "you are not in a team linked to voice", _tbl.rec = "join active teams", fail = true;
            if (fail) 
              return sendUDP(_tbl, rinfo.address, rinfo.port);
            client.users.fetch(userid)
            .then((user) => {
              let allchannels = voice[request.tag].channels.names;
              user.send(`Hey ${username}, you are now linked to auto-voice: ${args.sname} <=> ${channel.guild}. \nShould you join voicechat (${allchannels.join('/')}) on '${channel.guild.name}', you will automatically be switched to the respective team channel.\n\n**Tip:** you can bind the command to your F2-key to easily log in the next time you connect:` + '\n```/bind "F2" [servcmd voice ' + args.id + ']```\nHave fun! :smile:').catch((e) => out(e, 'Send'));
              log(`[Voice] Linked ${username} to ${args.cn}, team ${args.team} (${args.sname}).`);
              voice[request.tag].ids[args.id].discordid = userid;
              let tbl = {
                voice: true,
                cmd: "#vlinksuccess",
                direct: true,
                did: userid,
                dname: user.tag,
                id: args.id,
                cn: args.cn,
                team: args.team,
              };
              sendUDP(tbl, servers[request.tag].address, servers[request.tag].port);
              let oldteam = voice[request.tag].ids[args.id].team;
              voice[request.tag].ids[args.id].team = args.team;
              if (voice[request.tag].channels[args.team]) {
                synchronizeVoice(userid, voice[request.tag].channels[args.team].id)
                .catch((e) => { 
                  voice[request.tag].ids[args.id].team = oldteam;
                  out(e, 'Voice Move');
                });
              }

            })
            .catch((e) => out(e, 'User Fetch'))
          })
          .catch((e) => out(e, 'User Collect'))

          break;
        
        case 'request':
          for (const [id, obj] of Object.entries(voice[request.tag].ids)) {
            if (obj.cn == args.cn && !id.discordid) {
              delete voice[request.tag].ids.id;
              log(`[Voice] Update code: cn ${args.cn}.`);
            }
          }
          voice[request.tag].ids[args.id] = { tag: request.tag, cn: args.cn, team: args.team, sname: args.sname, guild: channel.guild.name };
          voice[request.tag].requests[args.id] = { tag: request.tag, sname: args.sname, guild: channel.guild.name };
          log(`[Voice] Link request from '${request.tag}': cn ${args.cn} -> id ${args.id}.`);
          break;

        case 'delrequests':
          for (const [stag, info] of Object.entries(voice)) {
            for (const [id, obj] of Object.entries(voice[stag].requests)) {
              delete voice[request.tag].requests[id];
            }
          }
          break;

        case 'login':
          if (!client.users.cache.get(args.did)) 
            return log(`[Voice] Login Fail for cn ${args.cn} @${args.sname}: Discord user for '${args.did}' not found!`);
          client.users.fetch(args.did)
          .then((duser) => {
            if (duser) {
              voice[request.tag].ids[args.id] = { discordid: duser.id, tag: request.tag, cn: args.cn, team: args.team, sname: args.sname, guild: channel.guild.name };
              let tbl = {
                voice: true,
                cmd: "#vlinksuccess",
                id: args.id,
                did: duser.id,
                dname: duser.tag,
                cn: args.cn,
                team: args.team
              };
              sendUDP(tbl, rinfo.address, rinfo.port);
              if (voice[request.tag].channels[args.team]) {
                synchronizeVoice(duser.id, voice[request.tag].channels[args.team].id)
                .catch((e) => out(e, 'Voice Move'));
              }
              log(`[Voice] Login: ${duser.username} || Server: ${args.sname} cn: ${args.cn}`);
            } 
          })
          .catch((e) => out(e));

          break;

        case 'disconnect':
          for (const [id, obj] of Object.entries(voice[request.tag].ids)) {
            if (obj.cn == args.cn) {
              delete voice[request.tag].ids[id];
              log(`[Voice] Removed cn ${args.cn} from id cache.`);
            }
          }
          break;

        case 'update':
          if (!args.voiceinfo) return;
          for (const [cn, team] of Object.entries(args.voiceinfo)) {
            for (const [id, obj] of Object.entries(voice[request.tag].ids)) {
              if ((obj.cn == cn) && voice[request.tag].ids[id].discordid) {
                let uoldteam = voice[request.tag].ids[id].team;
                voice[request.tag].ids[id].team = team;
                if (voice[request.tag].channels[team]) {
                  synchronizeVoice(obj.discordid, voice[request.tag].channels[team].id)
                  .catch((e) => { 
                    voice[request.tag].ids[id].team = uoldteam;
                    out(e, 'Voice Move 2');
                  });
                }
              }
            }
          }
          break;
      }
      
      break;
  }

  if ((description == '') && (cmsg == '')) return;

  if (description !== '') description = maskIPs(description);
  if (cmsg !== '') cmsg = maskIPs(cmsg);

  // Simple chat message
  if (cmsg && cmsg != '') {
    channel.send(cmsg).catch((e) => out(e, 'Send'));
    if (!isStatus) return;
  }

  let embed = new Discord.MessageEmbed()
    .setColor(color)
    .setAuthor(authorname)
    .setTitle(title)
    .setDescription(description)
    .setThumbnail(thumbnail)
    .addFields(fields)
    .setFooter(footer)
    .setTimestamp(timestamp);

  let all = embed;
  let files = [];
  all = { files: files, embed: embed };

  if (enableThumbnails && map && (map != '')) {
    if (!fs.existsSync(`./mapshots/${map}.jpg`)) {
      embed.attachFiles('./mapshots/cube.png').setThumbnail('attachment://cube.png');
    } else {
      embed.attachFiles(`./mapshots/${map}.jpg`).setThumbnail(`attachment://${map}.jpg`);
    }
  }

  let newgamerefresh = isNewgame;
  if (!enableThumbnails) newgamerefresh = false;

  // If enabled, update server status in status channel
  if (enableScoreboard && isStatus && hasScoreboardChannel) 
    return pushStatus(request.tag, channel, (isImportant && useEmbeds), scoreboardChannelID, all, scoreboard, newgamerefresh);

  if ((description == '') || (!useEmbeds && ((event != 'cmdbanlist') && (event != 'cmdhelp') && (event != 'cmdstatus')))) return;

  if (!useEmbeds) embed.setAuthor(preauthor + authorname);

  // Normal embed for the main channel, append an intermission scoreboard if given
  channel.send(all)
  .then(() => {
    if (scoreboard && !isNewgame && (isCommand || isImportant || (!isImportant && alwaysScoreboard))) 
      return channel.send(scoreboard);
  })
  .catch((e) => out(e, 'Send')); 
});

// init server
server.bind(relayPort, relayHost);


/***
    Create the Discord client that will listen to commands and send them to the gameserver.
***/

function getNick(user, guild) {
  const userDetails = guild.members.resolve(user.id);
  return (userDetails ? (userDetails.nickname || user.username) : user.username);
}

client.on('ready', () => log('[Discord] Successfully connected to discord!'));

client.on('error', (e) => out(e.message));

client.on('voiceStateUpdate', (oldm, newm) => {
  if (!newm.channelID) return;
  let newID = newm.channelID;
  for (const [tag] of Object.entries(servers)) {
    for (const [id, obj] of Object.entries(voice[tag].ids)) {
      if (obj.discordid && (obj.discordid == newm.id) && voice[tag].channels[obj.team] && (newID != voice[tag].channels[obj.team].id)) {
        synchronizeVoice(obj.discordid, voice[tag].channels[obj.team].id)
        .catch((e) => out(e, 'Voice Move'));
      }
    }
  }
});

client.on('message', (message) => {
  if((!message.channel.type == 'text') || (message.author.id == client.user.id)) return;  // only take commands from active channel
  if(channels[message.channel.id] && message.content.startsWith(prefix)) {
    const args = message.content.slice(prefix.length).split(' ');
    const cmd = args.shift().toLowerCase();
    if(cmd && cmd != ''){  // a command was issued
      if (cmd == 'embeds') {
        useEmbeds = !useEmbeds;
        message.channel.send(':warning: Embedded chat messages are now ' + (useEmbeds ? 'en' : 'dis') + 'abled for this bot. This does not affect the scoreboard.')
        .catch((e) => out(e, 'Send'));
        return
      }
      log(`[Command] ${getNick(message.author, message.guild)}: ${message}`);
      let address = channels[message.channel.id].address;
      let port = channels[message.channel.id].port;
      let tbl = {
        user: getNick(message.author, message.guild),
        userid: message.author.id,
        prefix: prefix,
        cmd: cmd,
        args: args.join(' ')
      };
      sendUDP(tbl, address, port);
    } 
  } else {
    let onlyrequest = false,
        id,
        tag,
        _pid;
    for (const [stag, info] of Object.entries(voice)) {
      for (const [pid, obj] of Object.entries(voice[stag].ids)) {
        if(pid.toString() == message.content) {
          tag = stag;
          id = voice[stag].ids[pid];
          _pid = pid;
        }
      }
      if (!id) {
        for (const [rid] of Object.entries(voice[stag].requests)) {
          if (rid.toString() == message.content) {
            tag = stag;
            id = voice[stag].requests[rid];
            _pid = rid
            onlyrequest = true
          }
        }
      }
    }
    if(!id) return;
    if(!id.discordid) {
      let allchannels = voice[tag].channels.names;
      message.channel.send(`Hey ${message.author.username}, you are now linked to auto-voice on ${id.sname}.\nShould you join voicechat (${allchannels.join('/')}) on '${id.guild}', you will automatically be switched to the respective team channel.\n\n**Tip:** you can bind the command to your F2-key to easily log in the next time you connect:` + '\n```/bind "F2" [servcmd voice ' + _pid + ']```\nHave fun! :smile:').catch((e) => out(e, 'Send'));
      if (onlyrequest) {
        let tbl = {
          voice: true,
          cmd: "#vreqsuccess",
          did: message.author.id,
          dname: message.author.username,
          id: message.content,
        };
        sendUDP(tbl, servers[id.tag].address, servers[id.tag].port);
        delete voice[tag].requests[_pid];
        onlyrequest = false;
        return;
      }
      log(`[Voice] Linked ${message.author.username} to ${id.cn}, team ${id.team} (${id.sname}).`);
      voice[tag].ids[message.content].discordid = message.author.id;
      let tbl = {
        voice: true,
        cmd: "#vlinksuccess",
        did: message.author.id,
        dname: message.author.tag,
        id: message.content,
        cn: id.cn,
        team: id.team,
      };
      sendUDP(tbl, servers[id.tag].address, servers[id.tag].port);
      let oldteam = voice[tag].ids[message.content].team;
      voice[tag].ids[message.content].team = id.team;
      if (voice[tag].channels[id.team]) {
        synchronizeVoice(message.author.id, voice[tag].channels[id.team].id)
        .catch((e) => { 
          voice[tag].ids[message.content].team = oldteam;
          out(e, 'Voice Move');
        });
      }
    } else {
      if(id.discordid == message.author.id) {
        return message.channel.send(`Hey ${message.author.username}, you are still linked to cn ${id.cn} on ${id.sname}.\nIf you don't want to be linked anymore, simply reconnect.`).catch((e) => out(e, 'Send'));
      }
    }
  }
});

// init client
client.login(discordToken)
.then(log('[Discord] Logging in...'))
.catch((e) => out(e, 'Login'));


// shutting down
function terminate() {
  let tbl = { cmd: "#nodeshutdown" };
  for (const [tag] of Object.entries(servers)) { sendUDP(tbl, servers[tag].address, servers[tag].port) }
  log('[Discord] Logging out and terminating bot...');
  client.destroy()
  process.exit();
}

process.on('SIGINT', terminate);
process.on('SIGTERM', terminate);

process.on('unhandledRejection', (reason, p) => {
  out(`Unhandled Rejection at: ${p}: ${reason}`, 'JavaScript');
});

