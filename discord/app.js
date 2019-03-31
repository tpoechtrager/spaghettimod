/***********************************************************************
    This NodeJS discord bot exchanges spaghettimod event messages 
    and discord commands. Launch this before launching spaghettimod.
***********************************************************************/

const fs = require('fs'),
      dgram = require('dgram'),
      Discord = require('discord.js');

const server = dgram.createSocket('udp4'),
      client = new Discord.Client();

const { bot, udp } = require('./config.json');

const { discordToken, commandPrefix: prefix, alerts, alertChannelID, enableThumbnails, enableGeoipflags, enableScoreboard, alwaysScoreboard} = bot,
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

function fill(s) {
  if (!s || (s == '')) return;
  return s.replace(/\u0020/g, '\u005F');
}


/***
    Create the UDP instance that will listen to gameserver messages to be relayed to discord.
***/

let servers = [],
    channels = [],
    statusIDs = [];

function updateServer(tag, channel, rinfo) {
  servers[tag] = { channel: channel, address: rinfo.address, port: rinfo.port };
  channels[channel] = {address: rinfo.address, port: rinfo.port};
}

function sendUDP(tbl, address, port) {
  let str = JSON.stringify(tbl);
  let buf = new Buffer(str);
  return server.send(buf, 0, buf.length, port, address, (e) => out(e, 'UDP'));
}

function canPurge(channel) {
  return channel.permissionsFor(client.user).has('READ_MESSAGE_HISTORY', 'MANAGE_MESSAGES');
}

let perms = [ 
  'SEND_MESSAGES',
  'VIEW_CHANNEL',
  'EMBED_LINKS',
  'READ_MESSAGE_HISTORY',
  'MANAGE_MESSAGES'
]
function missingAccess(channel) {
  if (alerts && !perms.includes('MENTION_EVERYONE')) perms.push('MENTION_EVERYONE');
  if (enableThumbnails && !perms.includes('ATTACH_FILES')) perms.push('ATTACH_FILES');
  let needed = [];
  perms.forEach((flag) => { if(!channel.permissionsFor(client.user).has(flag)) needed.push(flag) });
  return (needed.length ? needed.join(' and ') : false);
}

function purge(channelID, logpurge) {
  return new Promise((resolve, reject) => {
    let channel = client.channels.find(channel => channel.id === channelID);
    if (!channel) return reject('Purge Error: Channel not found.');
    if (!canPurge(channel))
      return reject(`Purge Error: I need Read Message History and Manage Masseges permission in #${channel.name}. I need those to clean up the channel.`);
    channel.fetchMessages({ limit: 100 })
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

function pushStatus(channel, important, scoreboardChannelID, embed, scoreboard, forcepurge) {
  let scoreboardChannel = client.channels.find(channel => channel.id === scoreboardChannelID);
  if (!scoreboardChannel) return;

  if (!forcepurge && statusIDs[scoreboardChannelID] && statusIDs[scoreboardChannelID].embed && statusIDs[scoreboardChannelID].scoreboard) {
    scoreboardChannel.fetchMessage(statusIDs[scoreboardChannelID].embed)
    .then((oldembed) => oldembed.edit(embed))
    .then(() => scoreboardChannel.fetchMessage(statusIDs[scoreboardChannelID].scoreboard))
    .then((oldscoreboard) => oldscoreboard.edit(scoreboard))
    .catch((e) => out(e, 'Scoreboard Edit'));
    if (important) channel.send(embed).catch((e) => out(e, 'Scoreboard Send'));
   return;
  }

  let embedid = '';
  purge(scoreboardChannelID)
  .then(() => scoreboardChannel.send(embed).catch((e) => out(e)))
  .then((embedmsg) => embedid = embedmsg.id)
  .then(() => scoreboardChannel.send(scoreboard))
  .then((sbmsg) => statusIDs[scoreboardChannelID] = { embed: embedid, scoreboard: sbmsg.id })
  .catch((e) => out(e, 'Scoreboard Send'));
  if (important) channel.send(embed).catch((e) => out(e, 'Scoreboard Send'));
  return;
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

  updateServer(request.tag, request.channel, rinfo);

  const channel = client.channels.find(channel => channel.id === servers[request.tag].channel);
  if (!channel) return;
  if (missingAccess(channel))
    return out(`I need ${missingAccess(channel)} permissions in #${channel.name}!`, 'Permissions');

  var hasScoreboardChannel = false;
  const scoreboardChannelID = request.scoreboardChannel;
  const scoreboardChannel = client.channels.find(channel => channel.id === scoreboardChannelID);
  if (scoreboardChannel && (scoreboardChannel != '')) {
    if (!missingAccess(scoreboardChannel)) {
      hasScoreboardChannel = true;
    } else {
      out(`I need ${missingAccess(scoreboardChannel)} permissions in #${scoreboardChannel.name}!`, 'Permissions');
    }
  }

  let cmsg = '',  
      color = 0x989898;
  let geoipflag = '';
  let author = { name: '', url: '', icon_url: '' };
  let title = '', 
      description = '',
      map = '';
  let thumbnail = { url: '' };
  let fields = [];
  let timestamp = '';
  let footer = { text: ''};
  let scoreboard = '';
  let isCommand = false, 
      isStatus = false,
      isImportant = false,
      isNewgame = false;
   
  switch (event) {

    case 'register':
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
      client.user.setActivity('Cube 2: Sauerbraten');
      log(`[Discord] Registered server '${args.server}' ['${request.tag}'] @${rinfo.address}:${rinfo.port}`);
      break;

    // Command echos

    case 'cmderr':
      cmsg = `<@${args.userid}>\n${args.msg}`;
      break;

    case 'cmdhelp':
      author.name = 'COMMANDS';
      description = `List of commands for '${args.server}'\nUsage: ${prefix}<command> <args> [<opt. args>]\n\u200b`;
      color = 0x6485ff;
      for (let cmd in args.list) {
        if (args.list.hasOwnProperty(cmd)) {
          fields.push({ name: prefix + args.list[cmd].name + ' ' + args.list[cmd].cmdargs, value: args.list[cmd].help });
        }
      }
      break;

    case 'cmdbanlist':
      author.name = `BANLIST ${addspaces(140)}\u200b`;
      let choice = (args.all ? `by banlist` : `in banlist '${args.blist.name}'`)
      description = `Enumerating bans ${choice}:\nExpire :: Range :: Reason`;
      color = 0x6485ff;
      if (args.all) {
        for (let banlist in args.lists) {
          if (args.lists.hasOwnProperty(banlist)) {
            fields.push({ name: args.lists[banlist].name, value: '```\n' + args.lists[banlist].list + '\n```' });
          }
        }
      } else {
        description += '```\n' + args.blist.list + '\n```'
      }
      break;

    case 'cmdstatus':
      author.name = 'SERVER STATUS';
      description = `Mode/map: **${args.modemap}**\nPlayers: **${args.players}**\n\n`;
      scoreboard = '```diff\nServing ' + args.players + ' (high frags/acc/tks are highlighted):\n\n' + args.str + '\n```';
      map = args.nmap;
      isCommand = true;
      color = 0x2aa198;
      timestamp = new Date();
      break;

    case 'cmdsay':
      cmsg = `${args.name}: [REMOTE] ${args.txt}`;
      break;

    case 'cmdstats':
      author.name = `STATS ${addspaces(100)}\u200b`;
      geoipflag = fill(args.country);
      description = `**Name:** ${args.name} (${args.cn})\n`;
      description += `**Location**: ${args.location}\n`;
      description += `**Playtime**: ${args.contime}\n\n`;
      description += '```diff\nStats: ' + args.stats + '\n```';
      color = 0xffa700;
      break;

    case 'cmdgetip':
      author.name = 'IP';
      geoipflag = fill(args.country);
      description = `**Name:** ${args.name} (${args.cn})\n`;
      description += `**Location**: ${args.location}\n`;
      description += `**Playtime**: ${args.contime}\n\n`;
      description += `**IP**: ${args.ip}\n\n`;
      color = 0xffa700;
      break;

    // Server broadcast

    case 'chat':
      cmsg = `${args.name} (${args.clientnum}): ${args.txt}`;
      break;

    case 'info':
      description = args.txt;
      break;

    case 'rename':
      author.name = 'RENAME';
      description = `${args.oldname} (${args.clientnum}) is now known as ${args.newname}`;
      color = 0x3a56bd;
      break;

    case 'connected':
      author.name = 'CONNECT';
      geoipflag = fill(args.country);
      description = `${args.name} (${args.clientnum}) connected from ${args.country}`;
      color = 0x6485ff;
      break;

    case 'disconnected':
      author.name = 'DISCONNECT';
      description = `${args.name} (${args.clientnum}) disconnected after ${args.contime}${args.reason}`;
      color = (args.iskick ? 0xff0000 : 0x0036f9);
      break;

    case 'master':
      author.name = 'MASTER';
      description = `${args.name} (${args.clientnum}) ${args.action}`;
      break;

    case 'kick':
      author.name = 'KICKBAN';
      description = args.str;
      color = 0xff0000;
      break;

    case 'newgame':
      author.name = 'NEW GAME';
      description = `Mode/map: **${args.modemap}**\nPlayers: **${args.players}**\n\n`;
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
      author.name = `SERVER STATUS ${args.progress}`;
      description = `Mode/map: **${args.modemap}**\nPlayers: **${args.players}**\n\n`;
      description += '_Updates every minute._';
      scoreboard = '```diff\nListing ' + args.players + ' (high acc/tks are highlighted):\n\n' + args.str + '\n```';
      map = args.nmap;
      isStatus = true;
      color = 0x2aa198;
      footer.text = 'Last update';
      timestamp = new Date();
      break;

    case 'intermission':
      author.name = 'INTERMISSION';
      description = `Mode/map: **${args.modemap}**\nPlayers: **${args.players}**\n\n`;
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
      let alertmsg = '\u200b\n```diff\n' + args.subject + '\n```\n_reported by ' + args.reporters + '_\n\nPlease join the server to evaluate.\n\u200b';
      const alert = new Discord.RichEmbed({
        color: 0xffa700, 
        author: { 
          name: `CHEATER REPORT: '${args.server}'${addspaces(70)}\u200b`
        }, 
        description: alertmsg, 
        timestamp: new Date()
      });
      if (alertChannelID && (alertChannelID != '')) {
        const achannel = client.channels.find(channel => channel.id === alertChannelID);
        if (achannel) {
          if (missingAccess(achannel))
            return out(`I need ${missingAccess(achannel)} permissions in #${achannel.name}!`, 'Permissions');
          achannel.send('@here', alert).catch((e) => out(e, 'Send'));
        } else {
          channel.send('@here', alert).catch((e) => out(e, 'Send'));
        }
      } else {
        channel.send('@here', alert).catch((e) => out(e, 'Send'));
      }
      break;
  }

  if ((description == '') && (cmsg == '')) return;

  let embed = new Discord.RichEmbed({
    color: color,
    author: author,
    title: title,
    description: description,
    thumbnail: thumbnail,
    fields: fields,
    footer: footer,
    timestamp: timestamp
  })

  let all = embed;
  let files = [];

  if (enableGeoipflags && (geoipflag != '')) { 
    if (!fs.existsSync(`./flags/${geoipflag}.png`)) {
      embed.author.icon_url = 'attachment://Unknown.png';
      files.push(new Discord.Attachment('./flags/Unknown.png'));
      all = { files: files, embed: embed };
    } else {
      embed.author.icon_url = `attachment://${geoipflag}.png`;
      files.push(new Discord.Attachment(`./flags/${geoipflag}.png`));
      all = { files: files, embed: embed };
    }
  }

  if (enableThumbnails && map && (map != '')) {
    if (!fs.existsSync(`./mapshots/${map}.jpg`)) {
      embed.thumbnail.url = 'attachment://cube.png';
      files.push(new Discord.Attachment('./mapshots/cube.png'));
      all = { files: files, embed: embed };
    } else {
      embed.thumbnail.url = `attachment://${map}.jpg`;
      files.push(new Discord.Attachment(`./mapshots/${map}.jpg`));
      all = { files: files, embed: embed };
    }
  }
  let newgamerefresh = isNewgame;
  if (!enableThumbnails) newgamerefresh = false;

  // Simple chat message
  if (cmsg && cmsg != '') 
    return channel.send(cmsg).catch((e) => out(e, 'Send'));

  // If enabled, update server status in status channel
  if (enableScoreboard && isStatus && hasScoreboardChannel) 
    return pushStatus(channel, isImportant, scoreboardChannelID, all, scoreboard, newgamerefresh);

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
  const userDetails = guild.members.get(user.id);
  return (userDetails ? (userDetails.nickname || user.username) : user.username);
}

client.on('ready', () => log('[Discord] Successfully connected to discord!'));

client.on('error', (e) => out(e.message));

client.on('message', (message) => {
  if(message.channel.type == 'text' && channels[message.channel.id]) {  // only take commands from active channel
    if((message.author.id !== client.user.id) && message.content.startsWith(prefix)) {
      const args = message.content.slice(prefix.length).split(' ');
      const cmd = args.shift().toLowerCase();
      if(cmd && cmd != ''){  // a command was issued
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
    }
  }
});

// init client
client.login(discordToken)
.then(log('[Discord] Logging in...'))
.catch((e) => out(e, 'Login'));


// shutting down
function terminate() {
  client.destroy()
  .then(() => {
    log('[Discord] Logging out and terminating bot...');
    process.exit();
  })
  .catch((e) => {
    out(e, 'Shutdown');
    process.exit();
  });
}

process.on('SIGINT', terminate);
process.on('SIGTERM', terminate);

process.on('unhandledRejection', (reason, p) => {
  out(`Unhandled Rejection at: ${p}: ${reason}`, 'JavaScript');
});

