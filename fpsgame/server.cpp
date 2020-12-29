#define ALLOC_ARRAYS
#include "game.h"
#include "spaghetti.h"

namespace game
{
    void parseoptions(vector<const char *> &args)
    {
        loopv(args)
#ifndef STANDALONE
            if(!game::clientoption(args[i]))
#endif
            if(!server::serveroption(args[i]))
                conoutf(CON_ERROR, "unknown command-line option: %s", args[i]);
    }

    const char *gameident() { return "fps"; }
}

VAR(regenbluearmour, 0, 1, 1);

extern ENetAddress masteraddress;
int MAXBOTS = 32;

namespace server
{
    int DEATHMILLIS = 300;

    struct clientinfo;

    struct gameevent
    {
        virtual ~gameevent() {}

        virtual bool flush(clientinfo *ci, int fmillis);
        virtual void process(clientinfo *ci) {}

        virtual bool keepable() const { return false; }
    };

    struct timedevent : gameevent
    {
        int millis;

        bool flush(clientinfo *ci, int fmillis);
    };

    struct hitinfo
    {
        int target;
        int lifesequence;
        int rays;
        float dist;
        vec dir;
    };

    struct shotevent : timedevent
    {
        int id, gun;
        vec from, to;
        vector<hitinfo> hits;

        void process(clientinfo *ci);
    };

    struct explodeevent : timedevent
    {
        int id, gun;
        vector<hitinfo> hits;

        bool keepable() const { return true; }

        void process(clientinfo *ci);
    };

    struct suicideevent : gameevent
    {
        void process(clientinfo *ci);
    };

    struct pickupevent : gameevent
    {
        int ent;

        void process(clientinfo *ci);
    };

    template <int N>
    struct projectilestate
    {
        using lua_array = ::lua_array<int, N>;
        lua_array projs;
        int numprojs;

        projectilestate() : numprojs(0) {}

        void reset() { numprojs = 0; }

        void add(int val)
        {
            if(numprojs>=N) numprojs = 0;
            projs[numprojs++] = val;
        }

        bool remove(int val)
        {
            loopi(numprojs) if(projs[i]==val)
            {
                projs[i] = projs[--numprojs];
                return true;
            }
            return false;
        }
    };

    struct gamestate : fpsstate
    {
        vec o;
        int state, editstate;
        int lastdeath, deadflush, lastspawn, lifesequence;
        int lastshot;
        projectilestate<8> rockets, grenades;
        int frags, flags, deaths, teamkills, shotdamage, damage, tokens;
        int lasttimeplayed, timeplayed;
        float effectiveness;

        gamestate() : state(CS_DEAD), editstate(CS_DEAD), lifesequence(0) {}

        bool isalive(int gamemillis)
        {
            return state==CS_ALIVE || (state==CS_DEAD && gamemillis - lastdeath <= DEATHMILLIS);
        }

        bool waitexpired(int gamemillis)
        {
            return gamemillis - lastshot >= gunwait;
        }

        void reset()
        {
            if(state!=CS_SPECTATOR) state = editstate = CS_DEAD;
            maxhealth = 100;
            rockets.reset();
            grenades.reset();

            timeplayed = 0;
            effectiveness = 0;
            frags = flags = deaths = teamkills = shotdamage = damage = tokens = 0;

            lastdeath = 0;

            respawn();
        }

        void respawn()
        {
            fpsstate::respawn();
            o = vec(-1e10f, -1e10f, -1e10f);
            deadflush = 0;
            lastspawn = -1;
            lastshot = 0;
            tokens = 0;
        }

        void reassign()
        {
            respawn();
            rockets.reset();
            grenades.reset();
        }
    };

    struct savedscore
    {
        uint ip;
        lua_string name;
        int frags, flags, deaths, teamkills, shotdamage, damage;
        int timeplayed;
        float effectiveness;
        spaghetti::extra extra;

        void save(gamestate &gs)
        {
            frags = gs.frags;
            flags = gs.flags;
            deaths = gs.deaths;
            teamkills = gs.teamkills;
            shotdamage = gs.shotdamage;
            damage = gs.damage;
            timeplayed = gs.timeplayed;
            effectiveness = gs.effectiveness;
        }

        void restore(gamestate &gs)
        {
            gs.frags = frags;
            gs.flags = flags;
            gs.deaths = deaths;
            gs.teamkills = teamkills;
            gs.shotdamage = shotdamage;
            gs.damage = damage;
            gs.timeplayed = timeplayed;
            gs.effectiveness = effectiveness;
        }
    };

    extern int gamemillis, nextexceeded;

    struct clientinfo
    {
        int clientnum, ownernum, connectmillis, sessionid, overflow;
        lua_string name, team, mapvote;
        int playermodel;
        int modevote;
        int privilege;
        bool connected, local, timesync;
        int gameoffset, lastevent, pushed, exceeded;
        gamestate state;
        vector<gameevent *> events;
        vector<uchar> position, messages;
        uchar *wsdata;
        int wslen;
        vector<clientinfo *> bots;
        int ping, aireinit;
        lua_string clientmap;
        int mapcrc;
        bool warned, gameclip;
        ENetPacket *getdemo, *getmap, *clipboard;
        int lastclipboard, needclipboard;
        int connectauth;
        uint authreq;
        lua_string authname, authdesc;
        void *authchallenge;
        int authkickvictim;
        const char *authkickreason;
        spaghetti::extra extra = spaghetti::extra(true);

        clientinfo() : getdemo(NULL), getmap(NULL), clipboard(NULL), authchallenge(NULL), authkickreason(NULL) { reset(); extra.init(); }
        ~clientinfo() { extra.fini(); events.deletecontents(); cleanclipboard(); cleanauth(); }

        void addevent(gameevent *e)
        {
            if(state.state==CS_SPECTATOR || events.length()>100) delete e;
            else events.add(e);
        }

        enum
        {
            PUSHMILLIS = 3000
        };

        int calcpushrange()
        {
            ENetPeer *peer = getclientpeer(ownernum);
            return PUSHMILLIS + (peer ? peer->roundTripTime + peer->roundTripTimeVariance : ENET_PEER_DEFAULT_ROUND_TRIP_TIME);
        }

        bool checkpushed(int millis, int range)
        {
            return millis >= pushed - range && millis <= pushed + range;
        }

        void scheduleexceeded()
        {
            if(state.state!=CS_ALIVE || !exceeded) return;
            int range = calcpushrange();
            if(!nextexceeded || exceeded + range < nextexceeded) nextexceeded = exceeded + range;
        }

        void setexceeded()
        {
            if(state.state==CS_ALIVE && !exceeded && !checkpushed(gamemillis, calcpushrange())) exceeded = gamemillis;
            scheduleexceeded(); 
        }
            
        void setpushed()
        {
            pushed = max(pushed, gamemillis);
            if(exceeded && checkpushed(exceeded, calcpushrange())) exceeded = 0;
        }
        
        bool checkexceeded()
        {
            return state.state==CS_ALIVE && exceeded && gamemillis > exceeded + calcpushrange();
        }

        void mapchange()
        {
            mapvote[0] = 0;
            modevote = INT_MAX;
            state.reset();
            events.deletecontents();
            overflow = 0;
            timesync = false;
            lastevent = 0;
            exceeded = 0;
            pushed = 0;
            clientmap[0] = '\0';
            mapcrc = 0;
            warned = false;
            gameclip = false;
        }

        void reassign()
        {
            state.reassign();
            events.deletecontents();
            timesync = false;
            lastevent = 0;
        }

        void cleanclipboard(bool fullclean = true)
        {
            if(clipboard) { if(--clipboard->referenceCount <= 0) enet_packet_destroy(clipboard); clipboard = NULL; }
            if(fullclean) lastclipboard = 0;
        }

        void cleanauthkick()
        {
            authkickvictim = -1;
            DELETEA(authkickreason);
        }

        void cleanauth(bool full = true)
        {
            authreq = 0;
            if(authchallenge) { freechallenge(authchallenge); authchallenge = NULL; }
            if(full) cleanauthkick();
        }

        void reset()
        {
            name[0] = team[0] = 0;
            playermodel = -1;
            privilege = PRIV_NONE;
            connected = local = false;
            connectauth = 0;
            position.setsize(0);
            messages.setsize(0);
            ping = 0;
            aireinit = 0;
            needclipboard = 0;
            cleanclipboard();
            cleanauth();
            mapchange();
        }

        int geteventmillis(int servmillis, int clientmillis)
        {
            if(!timesync || (events.empty() && state.waitexpired(servmillis)))
            {
                timesync = true;
                gameoffset = servmillis - clientmillis;
                return servmillis;
            }
            else return gameoffset + clientmillis;
        }
    };

    struct ban
    {
        int time, expire;
        uint ip;
    };

    namespace aiman
    {
        extern void removeai(clientinfo *ci);
        extern void clearai();
        extern void checkai();
        extern void reqadd(clientinfo *ci, int skill);
        extern void reqdel(clientinfo *ci);
        extern void setbotlimit(clientinfo *ci, int limit);
        extern void setbotbalance(clientinfo *ci, bool balance);
        extern void changemap();
        extern void addclient(clientinfo *ci);
        extern void changeteam(clientinfo *ci);
    }

    #define MM_MODE 0xF
    #define MM_AUTOAPPROVE 0x1000
    #define MM_PRIVSERV (MM_MODE | MM_AUTOAPPROVE)
    #define MM_PUBSERV ((1<<MM_OPEN) | (1<<MM_VETO))
    #define MM_COOPSERV (MM_AUTOAPPROVE | MM_PUBSERV | (1<<MM_LOCKED))

    bool notgotitems = true;        // true when map has changed and waiting for clients to send item
    int gamemode = 0;
    int gamemillis = 0, gamelimit = 0, nextexceeded = 0, gamespeed = 100;
    bool gamepaused = false, shouldstep = true;

    string smapname = "";
    int interm = 0;
    enet_uint32 lastsend = 0;
    int mastermode = MM_OPEN, mastermask = MM_PRIVSERV;
    stream *mapdata = NULL;

    vector<uint> allowedips;
    vector<ban> bannedips;

    void addban(uint ip, int expire)
    {
        allowedips.removeobj(ip);
        ban b;
        b.time = totalmillis;
        b.expire = totalmillis + expire;
        b.ip = ip;
        loopv(bannedips) if(bannedips[i].expire - b.expire > 0) { bannedips.insert(i, b); return; }
        bannedips.add(b);
    }

    vector<clientinfo *> connects, clients, bots;

    void kickclients(uint ip, clientinfo *actor = NULL, int priv = PRIV_NONE)
    {
        loopvrev(clients)
        {
            clientinfo &c = *clients[i];
            if(c.state.aitype != AI_NONE || c.privilege >= PRIV_ADMIN || c.local) continue;
            if(actor && ((c.privilege > priv && !actor->local) || c.clientnum == actor->clientnum)) continue;
            if(getclientip(c.clientnum) == ip)
            {
                spaghetti::simpleconstevent(spaghetti::hotstring::kick, actor, c);
                disconnect_client(c.clientnum, DISC_KICK);
            }
        }
    }
 
    struct maprotation
    {
        static int exclude;
        int modes;
        string map;
        
        int calcmodemask() const { return modes&(1<<NUMGAMEMODES) ? modes & ~exclude : modes; }
        bool hasmode(int mode, int offset = STARTGAMEMODE) const { return (calcmodemask() & (1 << (mode-offset))) != 0; }

        int findmode(int mode) const
        {
            if(!hasmode(mode)) loopi(NUMGAMEMODES) if(hasmode(i, 0)) return i+STARTGAMEMODE;
            return mode;
        }

        bool match(int reqmode, const char *reqmap) const
        {
            return hasmode(reqmode) && (!map[0] || !reqmap[0] || !strcmp(map, reqmap));
        }

        bool includes(const maprotation &rot) const
        {
            return rot.modes == modes ? rot.map[0] && !map[0] : (rot.modes & modes) == rot.modes;
        }
    };
    int maprotation::exclude = 0;
    vector<maprotation> maprotations;
    int curmaprotation = 0;

    VAR(lockmaprotation, 0, 0, 2);

    void maprotationreset()
    {
        maprotations.setsize(0);
        curmaprotation = 0;
        maprotation::exclude = 0;
    }

    void nextmaprotation()
    {
        curmaprotation++;
        if(maprotations.inrange(curmaprotation) && maprotations[curmaprotation].modes) return;
        do curmaprotation--;
        while(maprotations.inrange(curmaprotation) && maprotations[curmaprotation].modes);
        curmaprotation++;
    }

    int findmaprotation(int mode, const char *map)
    {
        for(int i = max(curmaprotation, 0); i < maprotations.length(); i++)
        {
            maprotation &rot = maprotations[i];
            if(!rot.modes) break;
            if(rot.match(mode, map)) return i;
        }
        int start;
        for(start = max(curmaprotation, 0) - 1; start >= 0; start--) if(!maprotations[start].modes) break;
        start++;
        for(int i = start; i < curmaprotation; i++)
        {
            maprotation &rot = maprotations[i];
            if(!rot.modes) break;
            if(rot.match(mode, map)) return i;
        }
        int best = -1;
        loopv(maprotations)
        {
            maprotation &rot = maprotations[i];
            if(rot.match(mode, map) && (best < 0 || maprotations[best].includes(rot))) best = i;
        }
        return best;
    }

    bool searchmodename(const char *haystack, const char *needle)
    {
        if(!needle[0]) return true;
        do
        {
            if(needle[0] != '.')
            {
                haystack = strchr(haystack, needle[0]);
                if(!haystack) break;
                haystack++;
            }
            const char *h = haystack, *n = needle+1;
            for(; *h && *n; h++)
            {
                if(*h == *n) n++;
                else if(*h != ' ') break; 
            }
            if(!*n) return true;
            if(*n == '.') return !*h;
        } while(needle[0] != '.');
        return false;
    }

    int genmodemask(vector<char *> &modes)
    {
        int modemask = 0;
        loopv(modes)
        {
            const char *mode = modes[i];
            int op = mode[0];
            switch(mode[0])
            {
                case '*':
                    modemask |= 1<<NUMGAMEMODES;
                    loopk(NUMGAMEMODES) if(m_checknot(k+STARTGAMEMODE, M_DEMO|M_EDIT|M_LOCAL)) modemask |= 1<<k;
                    continue;
                case '!':
                    mode++;
                    if(mode[0] != '?') break;
                case '?':
                    mode++;
                    loopk(NUMGAMEMODES) if(searchmodename(gamemodes[k].name, mode))
                    {
                        if(op == '!') modemask &= ~(1<<k);
                        else modemask |= 1<<k;
                    }
                    continue;
            }
            int modenum = INT_MAX;
            if(isdigit(mode[0])) modenum = atoi(mode);
            else loopk(NUMGAMEMODES) if(searchmodename(gamemodes[k].name, mode)) { modenum = k+STARTGAMEMODE; break; }
            if(!m_valid(modenum)) continue;
            switch(op)
            {
                case '!': modemask &= ~(1 << (modenum - STARTGAMEMODE)); break;
                default: modemask |= 1 << (modenum - STARTGAMEMODE); break;
            }
        }
        return modemask;
    }
         
    bool addmaprotation(int modemask, const char *map)
    {
        if(!map[0]) loopk(NUMGAMEMODES) if(modemask&(1<<k) && !m_check(k+STARTGAMEMODE, M_EDIT)) modemask &= ~(1<<k);
        if(!modemask) return false;
        if(!(modemask&(1<<NUMGAMEMODES))) maprotation::exclude |= modemask;
        maprotation &rot = maprotations.add();
        rot.modes = modemask;
        copystring(rot.map, map);
        return true;
    }
        
    void addmaprotations(lua_State* L)
    {
        int numargs = lua_gettop(L)&~1;
        const char *args[numargs];
        loopi(numargs) args[i] = luaL_tolstring(L, i + 1, 0);
        vector<char *> modes, maps;
        for(int i = 0; i + 1 < numargs; i += 2)
        {
            explodelist(args[i], modes);
            explodelist(args[i+1], maps);
            int modemask = genmodemask(modes);
            if(maps.length()) loopvj(maps) addmaprotation(modemask, maps[j]);
            else addmaprotation(modemask, "");
            modes.deletearrays();
            maps.deletearrays();
        }
        if(maprotations.length() && maprotations.last().modes)
        {
            maprotation &rot = maprotations.add();
            rot.modes = 0;
            rot.map[0] = '\0';
        }
    }
    
    COMMAND(maprotationreset, "");
    COMMANDN(maprotation, addmaprotations, "ss2V");

    struct demofile
    {
        string info;
        uchar *data;
        int len;
        ~demofile() { DELETEA(data); }
    };

    vector<demofile> demos;

    bool demonextmatch = false;
    stream *demotmp = NULL, *demorecord = NULL, *demoplayback = NULL;
    int nextplayback = 0, demomillis = 0;

    VAR(maxdemos, 0, 5, 25);
    VAR(maxdemosize, 0, 16, 31);
    VAR(restrictdemos, 0, 1, 1);
    VARF(autorecorddemo, 0, 0, 1, demonextmatch = autorecorddemo!=0);

    VAR(restrictpausegame, 0, 1, 1);
    VAR(restrictgamespeed, 0, 1, 1);

    SVAR(serverdesc, "");
    SVAR(serverpass, "");
    SVAR(adminpass, "");
    VARF(publicserver, 0, 0, 2, {
		switch(publicserver)
		{
			case 0: default: mastermask = MM_PRIVSERV; break;
			case 1: mastermask = MM_PUBSERV; break;
			case 2: mastermask = MM_COOPSERV; break;
		}
	});
    SVAR(servermotd, "");

    struct teamkillkick
    {
        int modes, limit, ban;

        bool match(int mode) const
        {
            return (modes&(1<<(mode-STARTGAMEMODE)))!=0;
        }

        bool includes(const teamkillkick &tk) const
        {
            return tk.modes != modes && (tk.modes & modes) == tk.modes;
        }
    };
    vector<teamkillkick> teamkillkicks;

    void teamkillkickreset()
    {
        teamkillkicks.setsize(0);
    }

    void addteamkillkick(const char *modestr, int limit, int ban)
    {
        vector<char *> modes;
        explodelist(modestr, modes);
        teamkillkick &kick = teamkillkicks.add();
        kick.modes = genmodemask(modes);
        kick.limit = limit;
        kick.ban = ban > 0 ? ban*60000 : (ban < 0 ? 0 : 30*60000);
        modes.deletearrays();
    }

    COMMAND(teamkillkickreset, "");
    COMMANDN(teamkillkick, addteamkillkick, "sii");

    struct teamkillinfo
    {
        uint ip;
        int teamkills;
    };
    vector<teamkillinfo> teamkills;
    bool shouldcheckteamkills = false;

    void addteamkill(clientinfo *actor, clientinfo *victim, int n)
    {
        if(!m_timed || actor->state.aitype != AI_NONE || actor->local || actor->privilege || (victim && victim->state.aitype != AI_NONE)) return;
        shouldcheckteamkills = true;
        uint ip = getclientip(actor->clientnum);
        loopv(teamkills) if(teamkills[i].ip == ip) 
        { 
            teamkills[i].teamkills += n;
            return;
        }
        teamkillinfo &tk = teamkills.add();
        tk.ip = ip;
        tk.teamkills = n;
    }

    void checkteamkills()
    {
        teamkillkick *kick = NULL;
        if(m_timed) loopv(teamkillkicks) if(teamkillkicks[i].match(gamemode) && (!kick || kick->includes(teamkillkicks[i])))
            kick = &teamkillkicks[i];
        if(kick) loopvrev(teamkills)
        {
            teamkillinfo &tk = teamkills[i];
            if(tk.teamkills >= kick->limit)
            {
                if(kick->ban > 0)
                {
                    const char* const type = "teamkill";
                    uint ip = tk.ip;
                    int time = kick->ban;
                    if(!spaghetti::simplehook(spaghetti::hotstring::addban, type, ip, time))
                        addban(ip, time);
                }
                kickclients(tk.ip);
                teamkills.removeunordered(i);
            }
        }
        shouldcheckteamkills = false;
    }

    void *newclientinfo() { return new clientinfo; }
    void deleteclientinfo(void *ci) { delete (clientinfo *)ci; }

    clientinfo *getinfo(int n)
    {
        if(n < MAXCLIENTS) return (clientinfo *)getclientinfo(n);
        n -= MAXCLIENTS;
        return bots.inrange(n) ? bots[n] : NULL;
    }

    uint mcrc = 0;
    vector<entity> ments;
    vector<server_entity> sents;
    vector<savedscore> scores;

    int msgsizelookup(int msg)
    {
        static int sizetable[NUMMSG] = { -1 };
        if(sizetable[0] < 0)
        {
            memset(sizetable, -1, sizeof(sizetable));
            for(const int *p = msgsizes; *p >= 0; p += 2) sizetable[p[0]] = p[1];
        }
        return msg >= 0 && msg < NUMMSG ? sizetable[msg] : -1;
    }

    const char *modename(int n, const char *unknown)
    {
        if(m_valid(n)) return gamemodes[n - STARTGAMEMODE].name;
        return unknown;
    }

    const char *mastermodename(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodenames)/sizeof(mastermodenames[0])) ? mastermodenames[n-MM_START] : unknown;
    }

    const char *privname(int type)
    {
        switch(type)
        {
            case PRIV_ADMIN: return "admin";
            case PRIV_AUTH: return "auth";
            case PRIV_MASTER: return "master";
            default: return "unknown";
        }
    }

    void sendservmsg(const char *s) { sendf(-1, 1, "ris", N_SERVMSG, s); }
    void sendservmsgf(const char *fmt, ...)
    {
         defvformatstring(s, fmt, fmt);
         sendf(-1, 1, "ris", N_SERVMSG, s);
    }

    void resetitems()
    {
        mcrc = 0;
        ments.setsize(0);
        sents.setsize(0);
        //cps.reset();
    }

    bool serveroption(const char *arg)
    {
        if(arg[0]=='-') switch(arg[1])
        {
            case 'n': setsvar("serverdesc", &arg[2]); return true;
            case 'y': setsvar("serverpass", &arg[2]); return true;
            case 'p': setsvar("adminpass", &arg[2]); return true;
            case 'o': setvar("publicserver", atoi(&arg[2])); return true;
        }
        return false;
    }

    void serverinit()
    {
        smapname[0] = '\0';
        resetitems();
    }

    int numclients(int exclude = -1, bool nospec = true, bool noai = true, bool priv = false)
    {
        int n = 0;
        loopv(clients) 
        {
            clientinfo *ci = clients[i];
            if(ci->clientnum!=exclude && (!nospec || ci->state.state!=CS_SPECTATOR || (priv && (ci->privilege || ci->local))) && (!noai || ci->state.aitype == AI_NONE)) n++;
        }
        return n;
    }

    bool duplicatename(clientinfo *ci, const char *name)
    {
        if(!name) name = ci->name;
        loopv(clients) if(clients[i]!=ci && !strcmp(name, clients[i]->name)) return true;
        return false;
    }

    const char *colorname(clientinfo *ci, const char *name = NULL)
    {
        if(!name) name = ci->name;
        if(name[0] && !duplicatename(ci, name) && ci->state.aitype == AI_NONE) return name;
        static string cname[3];
        static int cidx = 0;
        cidx = (cidx+1)%3;
        formatstring(cname[cidx], ci->state.aitype == AI_NONE ? "%s \fs\f5(%d)\fr" : "%s \fs\f5[%d]\fr", name, ci->clientnum);
        return cname[cidx];
    }

    struct servmode
    {
        virtual ~servmode() {}

        virtual void entergame(clientinfo *ci) {}
        virtual void leavegame(clientinfo *ci, bool disconnecting = false) {}

        virtual void moved(clientinfo *ci, const vec &oldpos, bool oldclip, const vec &newpos, bool newclip) {}
        virtual bool canspawn(clientinfo *ci, bool connecting = false) { return true; }
        virtual void spawned(clientinfo *ci) {}
        virtual int fragvalue(clientinfo *victim, clientinfo *actor)
        {
            if(victim==actor || isteam(victim->team, actor->team)) return -1;
            return 1;
        }
        virtual void died(clientinfo *victim, clientinfo *actor) {}
        virtual bool canchangeteam(clientinfo *ci, const char *oldteam, const char *newteam) { return true; }
        virtual void changeteam(clientinfo *ci, const char *oldteam, const char *newteam) {}
        virtual void initclient(clientinfo *ci, packetbuf &p, bool connecting) {}
        virtual void update() {}
        virtual void cleanup() {}
        virtual void setup() {}
        virtual void newmap() {}
        virtual void intermission() {}
        virtual bool hidefrags() { return false; }
        virtual int getteamscore(const char *team) { return 0; }
        virtual void getteamscores(vector<teamscore> &scores) {}
        virtual bool extinfoteam(const char *team, ucharbuf &p) { return false; }
        virtual void parseitems(const vector<servmodeitem>& items, bool commit) {}
    };

    #define SERVMODE 1
    #include "capture.h"
    #include "ctf.h"
    #include "collect.h"

    captureservmode capturemode;
    ctfservmode ctfmode;
    collectservmode collectmode;
    servmode *smode = NULL;

    bool canspawnitem(int type)
    {
        bool can = !m_noitems && (type>=I_SHELLS && type<=I_QUAD && (!m_noammo || type<I_SHELLS || type>I_CARTRIDGES));
        spaghetti::simpleevent(spaghetti::hotstring::canspawnitem, can, type);
        return can;
    }

    int spawntime(int type)
    {
        if(m_classicsp) return INT_MAX;
        int np = numclients(-1, true, false);
        np = np<3 ? 4 : (np>4 ? 2 : 3);         // spawn times are dependent on number of players
        int sec = 0;
        switch(type)
        {
            case I_SHELLS:
            case I_BULLETS:
            case I_ROCKETS:
            case I_ROUNDS:
            case I_GRENADES:
            case I_CARTRIDGES: sec = np*4; break;
            case I_HEALTH: sec = np*5; break;
            case I_GREENARMOUR: sec = 20; break;
            case I_YELLOWARMOUR: sec = 30; break;
            case I_BOOST: sec = 60; break;
            case I_QUAD: sec = 70; break;
        }
        int millis = sec*1000;
        spaghetti::simpleevent(spaghetti::hotstring::spawntime, millis);
        return millis;
    }

    bool delayspawn(int type)
    {
        bool delay;
        switch(type)
        {
            case I_GREENARMOUR:
            case I_YELLOWARMOUR:
                delay = !m_classicsp; break;
            case I_BOOST:
            case I_QUAD:
                delay = true; break;
            default:
                delay = false;
        }
        spaghetti::simpleevent(spaghetti::hotstring::delayspawn, delay);
        return delay;
    }
 
    bool pickup(int i, int sender)         // server side item pickup, acknowledge first client that gets it
    {
        clientinfo *ci = getinfo(sender);
        if(spaghetti::simplehook(spaghetti::hotstring::prepickup, i, sender, ci)) return false;
        if((m_timed && gamemillis>=gamelimit) || !sents.inrange(i) || !sents[i].spawned) return false;
        if(!ci || (!ci->local && !ci->state.canpickup(sents[i].type)))
        {
            sendf(sender, 1, "ri3", N_ITEMACC, i, -1);
            return false;
        }
        sents[i].spawned = false;
        sents[i].spawntime = spawntime(sents[i].type);
        sendf(-1, 1, "ri3", N_ITEMACC, i, sender);
        ci->state.pickup(sents[i].type);
        spaghetti::simpleconstevent(spaghetti::hotstring::pickup, i, sender, ci);
        return true;
    }

    static hashset<teaminfo> teaminfos;

    void clearteaminfo()
    {
        teaminfos.clear();
    }

    bool teamhasplayers(const char *team) { loopv(clients) if(!strcmp(clients[i]->team, team)) return true; return false; }

    bool pruneteaminfo()
    {
        int oldteams = teaminfos.numelems;
        enumerate(teaminfos, teaminfo, old,
            if(!old.frags && !teamhasplayers(old.team)) teaminfos.remove(old.team);
        );
        return teaminfos.numelems < oldteams;
    }

    teaminfo *addteaminfo(const char *team)
    {
        teaminfo *t = teaminfos.access(team);
        if(!t)
        {
            if(teaminfos.numelems >= MAXTEAMS && !pruneteaminfo()) return NULL;
            t = &teaminfos[team];
            copystring(t->team, team, sizeof(t->team));
            t->frags = 0;
        }
        return t;
    }

    clientinfo *choosebestclient(float &bestrank)
    {
        clientinfo *best = NULL;
        bestrank = -1;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->state.timeplayed<0) continue;
            float rank = ci->state.state!=CS_SPECTATOR ? ci->state.effectiveness/max(ci->state.timeplayed, 1) : -1;
            if(!best || rank > bestrank) { best = ci; bestrank = rank; }
        }
        return best;
    }

    VAR(persistteams, 0, 0, 1);

    void autoteam()
    {
        if(spaghetti::simplehook(spaghetti::hotstring::autoteam)) return;
        static const char * const teamnames[2] = {"good", "evil"};
        vector<clientinfo *> team[2];
        float teamrank[2] = {0, 0};
        for(int round = 0, remaining = clients.length(); remaining>=0; round++)
        {
            int first = round&1, second = (round+1)&1, selected = 0;
            while(teamrank[first] <= teamrank[second])
            {
                float rank;
                clientinfo *ci = choosebestclient(rank);
                if(!ci) break;
                if(smode && smode->hidefrags()) rank = 1;
                else if(selected && rank<=0) break;
                ci->state.timeplayed = -1;
                team[first].add(ci);
                if(rank>0) teamrank[first] += rank;
                selected++;
                if(rank<=0) break;
            }
            if(!selected) break;
            remaining -= selected;
        }
        loopi(sizeof(team)/sizeof(team[0]))
        {
            addteaminfo(teamnames[i]);
            loopvj(team[i])
            {
                clientinfo *ci = team[i][j];
                if(!strcmp(ci->team, teamnames[i])) continue;
                if(persistteams && ci->team[0] && (!smode || smode->canchangeteam(ci, teamnames[i], ci->team)))
                {
                    addteaminfo(ci->team);
                    continue;
                }
                copystring(ci->team, teamnames[i], MAXTEAMLEN+1);
                sendf(-1, 1, "riisi", N_SETTEAM, ci->clientnum, teamnames[i], -1);
            }
        }
    }

    struct teamrank
    {
        const char *name;
        float rank;
        int clients;

        teamrank(const char *name) : name(name), rank(0), clients(0) {}
    };

    const char *chooseworstteam(const char *suggest = NULL, clientinfo *exclude = NULL)
    {
        teamrank teamranks[2] = { teamrank("good"), teamrank("evil") };
        const int numteams = sizeof(teamranks)/sizeof(teamranks[0]);
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci==exclude || ci->state.aitype!=AI_NONE || ci->state.state==CS_SPECTATOR || !ci->team[0]) continue;
            ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
            ci->state.lasttimeplayed = lastmillis;

            loopj(numteams) if(!strcmp(ci->team, teamranks[j].name))
            {
                teamrank &ts = teamranks[j];
                ts.rank += ci->state.effectiveness/max(ci->state.timeplayed, 1);
                ts.clients++;
                break;
            }
        }
        teamrank *worst = &teamranks[numteams-1];
        loopi(numteams-1)
        {
            teamrank &ts = teamranks[i];
            if(smode && smode->hidefrags())
            {
                if(ts.clients < worst->clients || (ts.clients == worst->clients && ts.rank < worst->rank)) worst = &ts;
            }
            else if(ts.rank < worst->rank || (ts.rank == worst->rank && ts.clients < worst->clients)) worst = &ts;
        }
        return worst->name;
    }

    void prunedemos(int extra = 0)
    {
        int n = clamp(demos.length() + extra - maxdemos, 0, demos.length());
        if(n <= 0) return;
        loopi(n) delete[] demos[i].data;
        demos.remove(0, n);
    }
 
    void adddemo()
    {
        if(!demotmp) return;
        int len = (int)min(demotmp->size(), stream::offset((maxdemosize<<20) + 0x10000));
        demofile &d = demos.add();
        time_t t = time(NULL);
        char *timestr = ctime(&t), *trim = timestr + strlen(timestr);
        while(trim>timestr && iscubespace(*--trim)) *trim = '\0';
        formatstring(d.info, "%s: %s, %s, %.2f%s", timestr, modename(gamemode), smapname, len > 1024*1024 ? len/(1024*1024.f) : len/1024.0f, len > 1024*1024 ? "MB" : "kB");
        sendservmsgf("demo \"%s\" recorded", d.info);
        d.data = new uchar[len];
        d.len = len;
        demotmp->seek(0, SEEK_SET);
        demotmp->read(d.data, len);
        DELETEP(demotmp);
    }
        
    void enddemorecord()
    {
        if(!demorecord) return;

        DELETEP(demorecord);

        if(!demotmp) return;
        if(!maxdemos || !maxdemosize) { DELETEP(demotmp); return; }

        prunedemos(1);
        adddemo();
        spaghetti::simpleconstevent(spaghetti::hotstring::enddemorecord);
    }

    void writedemo(int chan, void *data, int len)
    {
        if(!demorecord) return;
        int stamp[3] = { gamemillis, chan, len };
        lilswap(stamp, 3);
        demorecord->write(stamp, sizeof(stamp));
        demorecord->write(data, len);
        if(demorecord->rawtell() >= (maxdemosize<<20)) enddemorecord();
    }

    void recordpacket(int chan, void *_data, int len)
    {
        bool flush = false;
        std::string data((char*)_data, len);
        bool skip = spaghetti::simplehook(spaghetti::hotstring::recordpacket, flush, data);
        if(!skip) writedemo(chan, (void*)data.data(), data.size());
        if(flush && demorecord) demorecord->flush();
    }

    int welcomepacket(packetbuf &p, clientinfo *ci);
    void sendwelcome(clientinfo *ci);

    void setupdemorecord()
    {
        if(!m_mp(gamemode) || m_edit) return;

        std::string filename = "demorecord";
        spaghetti::simpleevent(spaghetti::hotstring::setupdemorecord, filename);
        demotmp = (filename == "demorecord" ? opentempfile : openfile)(filename.c_str(), "w+b");
        if(!demotmp)
        {
            spaghetti::simpleconstevent(spaghetti::hotstring::enddemorecord);
            return;
        }

        stream *f = opengzfile(NULL, "wb", demotmp);
        if(!f)
        {
            spaghetti::simpleconstevent(spaghetti::hotstring::enddemorecord);
            DELETEP(demotmp);
            return;
        }

        sendservmsg("recording demo");

        demorecord = f;

        demoheader hdr;
        memcpy(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic));
        hdr.version = DEMO_VERSION;
        hdr.protocol = PROTOCOL_VERSION;
        lilswap(&hdr.version, 2);
        demorecord->write(&hdr, sizeof(demoheader));

        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        welcomepacket(p, NULL);
        writedemo(1, p.buf, p.len);
    }

    void listdemos(int cn)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_SENDDEMOLIST);
        putint(p, demos.length());
        loopv(demos) sendstring(demos[i].info, p);
        sendpacket(cn, 1, p.finalize());
    }

    void cleardemos(int n)
    {
        if(!n)
        {
            demos.shrink(0);
            sendservmsg("cleared all demos");
        }
        else if(demos.inrange(n-1))
        {
            delete[] demos[n-1].data;
            demos.remove(n-1);
            sendservmsgf("cleared demo %d", n);
        }
    }

    static void freegetmap(ENetPacket *packet)
    {
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->getmap == packet) ci->getmap = NULL;
        }
    }

    static void freegetdemo(ENetPacket *packet)
    {
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->getdemo == packet) ci->getdemo = NULL;
        }
    }

    void senddemo(clientinfo *ci, int num, int tag)
    {
        if(ci->getdemo) return;
        if(!num) num = demos.length();
        if(!demos.inrange(num-1)) return;
        demofile &d = demos[num-1];
        if((ci->getdemo = sendf(ci->clientnum, 2, "riim", N_SENDDEMO, tag, d.len, d.data)))
            ci->getdemo->freeCallback = freegetdemo;
    }

    void enddemoplayback()
    {
        if(!demoplayback) return;
        DELETEP(demoplayback);

        loopv(clients) sendf(clients[i]->clientnum, 1, "ri3", N_DEMOPLAYBACK, 0, clients[i]->clientnum);

        sendservmsg("demo playback finished");

        loopv(clients) sendwelcome(clients[i]);
    }

    SVARP(demodir, "demo");

    const char *getdemofile(const char *file, bool init)
    {
        if(!demodir[0]) return NULL;
        static string buf;
        copystring(buf, demodir);
        int dirlen = strlen(buf);
        if(buf[dirlen] != '/' && buf[dirlen] != '\\' && dirlen+1 < (int)sizeof(buf)) { buf[dirlen++] = '/'; buf[dirlen] = '\0'; }
        if(init)
        {
            const char *dir = findfile(buf, "w");
            if(!fileexists(dir, "w")) createdir(dir);
        }
        concatstring(buf, file);
        return buf;
    }

    void setupdemoplayback()
    {
        if(demoplayback) return;
        demoheader hdr;
        string msg;
        msg[0] = '\0';
        string file;
        copystring(file, smapname);
        int len = strlen(file);
        if(len < 4 || strcasecmp(&file[len-4], ".dmo")) concatstring(file, ".dmo");
        if(const char *buf = getdemofile(file, false)) demoplayback = opengzfile(buf, "rb");
        if(!demoplayback) demoplayback = opengzfile(file, "rb");
        if(!demoplayback) formatstring(msg, "could not read demo \"%s\"", file);
        else if(demoplayback->read(&hdr, sizeof(demoheader))!=sizeof(demoheader) || memcmp(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic)))
            formatstring(msg, "\"%s\" is not a demo file", file);
        else
        {
            lilswap(&hdr.version, 2);
            if(hdr.version!=DEMO_VERSION) formatstring(msg, "demo \"%s\" requires an %s version of Cube 2: Sauerbraten", file, hdr.version<DEMO_VERSION ? "older" : "newer");
            else if(hdr.protocol!=PROTOCOL_VERSION) formatstring(msg, "demo \"%s\" requires an %s version of Cube 2: Sauerbraten", file, hdr.protocol<PROTOCOL_VERSION ? "older" : "newer");
        }
        if(msg[0])
        {
            DELETEP(demoplayback);
            sendservmsg(msg);
            return;
        }

        sendservmsgf("playing demo \"%s\"", file);

        demomillis = 0;
        sendf(-1, 1, "ri3", N_DEMOPLAYBACK, 1, -1);

        if(demoplayback->read(&nextplayback, sizeof(nextplayback))!=sizeof(nextplayback))
        {
            enddemoplayback();
            return;
        }
        lilswap(&nextplayback, 1);
    }

    void readdemo()
    {
        if(!demoplayback) return;
        demomillis += curtime;
        while(demomillis>=nextplayback)
        {
            int chan, len;
            if(demoplayback->read(&chan, sizeof(chan))!=sizeof(chan) ||
               demoplayback->read(&len, sizeof(len))!=sizeof(len))
            {
                enddemoplayback();
                return;
            }
            lilswap(&chan, 1);
            lilswap(&len, 1);
            ENetPacket *packet = enet_packet_create(NULL, len+1, 0);
            if(!packet || demoplayback->read(packet->data+1, len)!=size_t(len))
            {
                if(packet) enet_packet_destroy(packet);
                enddemoplayback();
                return;
            }
            packet->data[0] = N_DEMOPACKET;
            packet->referenceCount++;
            sendpacket(-1, chan, packet);
            if(!--packet->referenceCount) enet_packet_destroy(packet);
            if(!demoplayback) break;
            if(demoplayback->read(&nextplayback, sizeof(nextplayback))!=sizeof(nextplayback))
            {
                enddemoplayback();
                return;
            }
            lilswap(&nextplayback, 1);
        }
    }

    void stopdemo()
    {
        if(m_demo) enddemoplayback();
        else enddemorecord();
    }

    void pausegame(bool val, clientinfo *ci = NULL)
    {
        if(gamepaused==val) return;
        gamepaused = val;
        sendf(-1, 1, "riii", N_PAUSEGAME, gamepaused ? 1 : 0, ci ? ci->clientnum : -1);
        spaghetti::simpleconstevent(spaghetti::hotstring::pausegame, val);
    }

    void checkpausegame()
    {
        if(!gamepaused) return;
        if(spaghetti::simplehook(spaghetti::hotstring::checkpausegame)) return;
        int admins = 0;
        loopv(clients) if(clients[i]->privilege >= (restrictpausegame ? PRIV_ADMIN : PRIV_MASTER) || clients[i]->local) admins++;
        if(!admins) pausegame(false);
    }

    void forcepaused(bool paused)
    {
        pausegame(paused);
    }

    bool ispaused() { return gamepaused; }

    void changegamespeed(int val, clientinfo *ci = NULL)
    {
        val = clamp(val, 10, 1000);
        if(gamespeed==val) return;
        gamespeed = val;
        sendf(-1, 1, "riii", N_GAMESPEED, gamespeed, ci ? ci->clientnum : -1);
    }

    void forcegamespeed(int speed)
    {
        changegamespeed(speed);
    }

    int scaletime(int t) { return t*gamespeed; }

    SVAR(serverauth, "");

    struct userkey
    {
        const char *name;
        const char *desc;
        
        userkey() : name(NULL), desc(NULL) {}
        userkey(const char *name, const char *desc) : name(name), desc(desc) {}
    };

    static inline uint hthash(const userkey &k) { return ::hthash(k.name); }
    static inline bool htcmp(const userkey &x, const userkey &y) { return !strcmp(x.name, y.name) && !strcmp(x.desc, y.desc); }

    struct userinfo : userkey
    {
        void *pubkey;
        int privilege;

        userinfo() : pubkey(NULL), privilege(PRIV_NONE) {}
        ~userinfo() { delete[] name; delete[] desc; if(pubkey) freepubkey(pubkey); }
    };
    hashset<userinfo> users;

    void adduser(const char *name, const char *desc, const char *pubkey, const char *priv)
    {
        userkey key(name, desc);
        userinfo &u = users[key];
        if(u.pubkey) { freepubkey(u.pubkey); u.pubkey = NULL; }
        if(!u.name) u.name = newstring(name);
        if(!u.desc) u.desc = newstring(desc);
        u.pubkey = parsepubkey(pubkey);
        switch(priv[0])
        {
            case 'a': case 'A': u.privilege = PRIV_ADMIN; break;
            case 'm': case 'M': default: u.privilege = PRIV_AUTH; break;
            case 'n': case 'N': u.privilege = PRIV_NONE; break;
        }
    }
    COMMAND(adduser, "ssss");

    void clearusers()
    {
        users.clear();
    }
    COMMAND(clearusers, "");

    void hashpassword(int cn, int sessionid, const char *pwd, char *result, int maxlen)
    {
        char buf[2*sizeof(string)];
        formatstring(buf, "%d %d ", cn, sessionid);
        concatstring(buf, pwd, sizeof(buf));
        if(!hashstring(buf, result, maxlen)) *result = '\0';
    }

    bool checkpassword(clientinfo *ci, const char *wanted, const char *given)
    {
        string hash;
        hashpassword(ci->clientnum, ci->sessionid, wanted, hash, sizeof(hash));
        return !strcmp(hash, given);
    }

    void revokemaster(clientinfo *ci)
    {
        ci->privilege = PRIV_NONE;
        if(ci->state.state==CS_SPECTATOR && !ci->local) aiman::removeai(ci);
    }

    extern void connected(clientinfo *ci);

    bool setmaster(clientinfo *ci, bool val, const char *pass = "", const char *authname = NULL, const char *authdesc = NULL, int authpriv = PRIV_MASTER, bool force = false, bool trial = false)
    {
        if(authname && !val) return false;
        const char *name = "";
        bool privchanged = false;
        int wantpriv = val ? (ci->local || (adminpass[0] && checkpassword(ci, adminpass, pass)) ? PRIV_ADMIN : authpriv) : 0;
        if(val)
        {
            if(wantpriv == PRIV_MASTER && !force)
            {
                if(ci->state.state==CS_SPECTATOR) 
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Spectators may not claim master.");
                    return false;
                }
                loopv(clients) if(ci!=clients[i] && clients[i]->privilege)
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Master is already claimed.");
                    return false;
                }
                if(!authname && !(mastermask&MM_AUTOAPPROVE) && !ci->privilege && !ci->local)
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "This server requires you to use the \"/auth\" command to claim master.");
                    return false;
                }
            }
            if(trial) return true;
            if(ci->privilege < wantpriv) privchanged = true, ci->privilege = wantpriv;
            name = privname(ci->privilege);
        }
        else
        {
            if(!ci->privilege) return false;
            if(trial) return true;
            name = privname(ci->privilege);
            revokemaster(ci);
            privchanged = true;
        }
        if(!spaghetti::simplehook(spaghetti::hotstring::checkmastermode))
        {
            bool hasmaster = false;
            loopv(clients) if(clients[i]->local || clients[i]->privilege >= PRIV_MASTER) hasmaster = true;
            if(!hasmaster && mastermode != MM_OPEN)
            {
                mastermode = MM_OPEN;
                allowedips.shrink(0);
                spaghetti::simpleconstevent(spaghetti::hotstring::mastermode, ci);
            }
        }
        string msg;
        if(privchanged)
        {
            if(val && authname)
            {
                if(authdesc && authdesc[0]) formatstring(msg, "%s claimed %s as '\fs\f5%s\fr' [\fs\f0%s\fr]", colorname(ci), name, authname, authdesc);
                else formatstring(msg, "%s claimed %s as '\fs\f5%s\fr'", colorname(ci), name, authname);
            }
            else formatstring(msg, "%s %s %s", colorname(ci), val ? "claimed" : "relinquished", name);
            packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
            putint(p, N_SERVMSG);
            sendstring(msg, p);
            putint(p, N_CURRENTMASTER);
            putint(p, mastermode);
            loopv(clients) if(clients[i]->privilege >= PRIV_MASTER)
            {
                putint(p, clients[i]->clientnum);
                putint(p, clients[i]->privilege);
            }
            putint(p, -1);
            sendpacket(-1, 1, p.finalize());
            checkpausegame();
        }
        int privilege = wantpriv;
        spaghetti::simpleconstevent(spaghetti::hotstring::master, ci, privilege, authname, authdesc);
        return true;
    }

    bool trykick(clientinfo *ci, int victim, const char *reason = NULL, const char *authname = NULL, const char *authdesc = NULL, int authpriv = PRIV_NONE, bool trial = false)
    {
        int priv = ci->privilege;
        if(authname)
        {
            if(priv >= authpriv || ci->local) authname = authdesc = NULL;
            else priv = authpriv;
        }
        clientinfo *vinfo = (clientinfo *)getclientinfo(victim);
        bool cankick = false;
        if(spaghetti::simplehook(spaghetti::hotstring::trykick, ci, victim, vinfo, reason, authname, authdesc, authpriv, trial, cankick)) return cankick;
        if((priv || ci->local) && ci->clientnum!=victim)
        {
            if(vinfo && vinfo->connected && (priv >= vinfo->privilege || ci->local) && vinfo->privilege < PRIV_ADMIN && !vinfo->local)
            {
                if(trial) return true;
                string kicker;
                if(authname)
                {
                    if(authdesc && authdesc[0]) formatstring(kicker, "%s as '\fs\f5%s\fr' [\fs\f0%s\fr]", colorname(ci), authname, authdesc);
                    else formatstring(kicker, "%s as '\fs\f5%s\fr'", colorname(ci), authname);
                }
                else copystring(kicker, colorname(ci));
                if(reason && reason[0]) sendservmsgf("%s kicked %s because: %s", kicker, colorname(vinfo), reason);
                else sendservmsgf("%s kicked %s", kicker, colorname(vinfo));
                uint ip = getclientip(victim);
                const char* const type = "kick";
                int time = 4*60*60000;
                if(!spaghetti::simplehook(spaghetti::hotstring::addban, type, ip, time, ci, reason, authname, authdesc)) addban(ip, time);
                kickclients(ip, ci, priv);
            }
        }
        return false;
    }

    savedscore *findscore(clientinfo *ci, bool insert)
    {
        uint ip = getclientip(ci->clientnum);
        if(!ip && !ci->local) return 0;
        if(!insert)
        {
            loopv(clients)
            {
                clientinfo *oi = clients[i];
                if(oi->clientnum != ci->clientnum && getclientip(oi->clientnum) == ip && !strcmp(oi->name, ci->name))
                {
                    oi->state.timeplayed += lastmillis - oi->state.lasttimeplayed;
                    oi->state.lasttimeplayed = lastmillis;
                    static savedscore curscore;
                    curscore.extra.fini();
                    curscore.extra.init();
                    const bool dummy = true;
                    {
                        auto& sc = curscore;
                        auto ci = oi;
                        if(!spaghetti::simplehook(spaghetti::hotstring::savegamestate, sc, ci, dummy))
                            curscore.save(oi->state);
                    }
                    return &curscore;
                }
            }
        }
        loopv(scores)
        {
            savedscore &sc = scores[i];
            if(sc.ip == ip && !strcmp(sc.name, ci->name)) return &sc;
        }
        if(!insert) return 0;
        savedscore &sc = scores.add();
        sc.ip = ip;
        copystring(sc.name, ci->name);
        return &sc;
    }

    void savescore(clientinfo *ci)
    {
        savedscore *sc = findscore(ci, true);
        if(sc)
        {
            const bool dummy = false;
            if(spaghetti::simplehook(spaghetti::hotstring::savegamestate, sc, ci, dummy)) return;
            sc->save(ci->state);
        }
    }

    static struct msgfilter
    {
        uchar msgmask[NUMMSG];

        msgfilter(int msg, ...)
        {
            memset(msgmask, 0, sizeof(msgmask));
            va_list msgs;
            va_start(msgs, msg);
            for(uchar val = 1; msg < NUMMSG; msg = va_arg(msgs, int))
            {
                if(msg < 0) val = uchar(-msg);
                else msgmask[msg] = val;
            }
            va_end(msgs);
        }

        uchar operator[](int msg) const { return msg >= 0 && msg < NUMMSG ? msgmask[msg] : 0; }
    } msgfilter(-1, N_CONNECT, N_SERVINFO, N_INITCLIENT, N_WELCOME, N_MAPCHANGE, N_SERVMSG, N_DAMAGE, N_HITPUSH, N_SHOTFX, N_EXPLODEFX, N_DIED, N_SPAWNSTATE, N_FORCEDEATH, N_TEAMINFO, N_ITEMACC, N_ITEMSPAWN, N_TIMEUP, N_CDIS, N_CURRENTMASTER, N_PONG, N_RESUME, N_BASESCORE, N_BASEINFO, N_BASEREGEN, N_ANNOUNCE, N_SENDDEMOLIST, N_SENDDEMO, N_DEMOPLAYBACK, N_SENDMAP, N_DROPFLAG, N_SCOREFLAG, N_RETURNFLAG, N_RESETFLAG, N_INVISFLAG, N_CLIENT, N_AUTHCHAL, N_INITAI, N_EXPIRETOKENS, N_DROPTOKENS, N_STEALTOKENS, N_DEMOPACKET, -2, N_REMIP, N_NEWMAP, N_GETMAP, N_SENDMAP, N_CLIPBOARD, -3, N_EDITENT, N_EDITF, N_EDITT, N_EDITM, N_FLIP, N_COPY, N_PASTE, N_ROTATE, N_REPLACE, N_DELCUBE, N_EDITVAR, N_EDITVSLOT, N_UNDO, N_REDO, -4, N_POS, NUMMSG),
      connectfilter(-1, N_CONNECT, -2, N_AUTHANS, -3, N_PING, NUMMSG);

    int checktype(int type, clientinfo *ci)
    {
        if(ci)
        {
            if(!ci->connected) switch(connectfilter[type])
            {
                // allow only before authconnect
                case 1: return !ci->connectauth ? type : -1;
                // allow only during authconnect
                case 2: return ci->connectauth ? type : -1;
                // always allow
                case 3: return type;
                // never allow
                default: return -1;
            }
            if(ci->local) return type;
        }
        switch(msgfilter[type])
        {
            // server-only messages
            case 1: return ci ? -1 : type;
            // only allowed in coop-edit
            case 2: if(m_edit) break; return -1;
            // only allowed in coop-edit, no overflow check
            case 3: return m_edit ? type : -1;
            // no overflow check
            case 4: return type;
        }
        if(ci && ++ci->overflow >= 200) return -2;
        return type;
    }

    struct worldstate
    {
        int uses, len;
        uchar *data;

        worldstate() : uses(0), len(0), data(NULL) {}

        void setup(int n) { len = n; data = new uchar[n]; }
        void cleanup() { DELETEA(data); len = 0; }
        bool contains(const uchar *p) const { return p >= data && p < &data[len]; }
    };
    vector<worldstate> worldstates;
    bool reliablemessages = false;

    worldstate& allocworldstate()
    {
        loopv(worldstates) if(!worldstates[i].data) return worldstates[i];
        return worldstates.add();
    }

    void cleanworldstate(ENetPacket *packet)
    {
        loopv(worldstates)
        {
            worldstate &ws = worldstates[i];
            if(!ws.contains(packet->data)) continue;
            ws.uses--;
            if(ws.uses <= 0) ws.cleanup();
            break;
        }
    }

    void flushclientposition(clientinfo &ci)
    {
        if(ci.position.empty() || (!hasnonlocalclients() && !demorecord)) return;
        packetbuf p(ci.position.length(), 0);
        p.put(ci.position.getbuf(), ci.position.length());
        ci.position.setsize(0);
        sendpacket(-1, 0, p.finalize(), ci.ownernum);
    }

    static void sendpositions(worldstate &ws, ucharbuf &wsbuf)
    {
        if(wsbuf.empty()) return;
        int wslen = wsbuf.length();
        recordpacket(0, wsbuf.buf, wslen);
        wsbuf.put(wsbuf.buf, wslen);
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            if(ci.state.aitype != AI_NONE || !allowbroadcast(ci.clientnum)) continue;
            uchar *data = wsbuf.buf;
            int size = wslen;
            if(ci.wsdata >= wsbuf.buf) { data = ci.wsdata + ci.wslen; size -= ci.wslen; }
            if(size <= 0) continue;
            ENetPacket *packet = enet_packet_create(data, size, ENET_PACKET_FLAG_NO_ALLOCATE);
            packet->referenceCount++;
            sendpacket(ci.clientnum, 0, packet);
            if(--packet->referenceCount) { ws.uses++; packet->freeCallback = cleanworldstate; }
            else enet_packet_destroy(packet);
        }
        wsbuf.offset(wsbuf.length());
    }

    static inline void addposition(worldstate &ws, ucharbuf &wsbuf, int mtu, clientinfo &bi, clientinfo &ci)
    {
        if(bi.position.empty()) return;
        if(wsbuf.length() + bi.position.length() > mtu) sendpositions(ws, wsbuf);
        int offset = wsbuf.length();
        wsbuf.put(bi.position.getbuf(), bi.position.length());
        bi.position.setsize(0);
        int len = wsbuf.length() - offset;
        if(ci.wsdata < wsbuf.buf) { ci.wsdata = &wsbuf.buf[offset]; ci.wslen = len; }
        else ci.wslen += len;
    }

    static void sendmessages(worldstate &ws, ucharbuf &wsbuf)
    {
        if(wsbuf.empty()) return;
        int wslen = wsbuf.length();
        recordpacket(1, wsbuf.buf, wslen);
        wsbuf.put(wsbuf.buf, wslen);
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            if(ci.state.aitype != AI_NONE || !allowbroadcast(ci.clientnum)) continue;
            uchar *data = wsbuf.buf;
            int size = wslen;
            if(ci.wsdata >= wsbuf.buf) { data = ci.wsdata + ci.wslen; size -= ci.wslen; }
            if(size <= 0) continue;
            ENetPacket *packet = enet_packet_create(data, size, (reliablemessages ? ENET_PACKET_FLAG_RELIABLE : 0) | ENET_PACKET_FLAG_NO_ALLOCATE);
            packet->referenceCount++;
            sendpacket(ci.clientnum, 1, packet);
            if(--packet->referenceCount) { ws.uses++; packet->freeCallback = cleanworldstate; }
            else enet_packet_destroy(packet);
        }
        wsbuf.offset(wsbuf.length());
    }

    static inline void addmessages(worldstate &ws, ucharbuf &wsbuf, int mtu, clientinfo &bi, clientinfo &ci)
    {
        if(bi.messages.empty()) return;
        if(wsbuf.length() + 10 + bi.messages.length() > mtu) sendmessages(ws, wsbuf);
        int offset = wsbuf.length();
        putint(wsbuf, N_CLIENT);
        putint(wsbuf, bi.clientnum);
        putuint(wsbuf, bi.messages.length());
        wsbuf.put(bi.messages.getbuf(), bi.messages.length());
        bi.messages.setsize(0);
        int len = wsbuf.length() - offset;
        if(ci.wsdata < wsbuf.buf) { ci.wsdata = &wsbuf.buf[offset]; ci.wslen = len; }
        else ci.wslen += len;
    }

    bool buildworldstate()
    {
        int wsmax = 0;
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            ci.overflow = 0;
            ci.wsdata = NULL;
            wsmax += ci.position.length();
            if(ci.messages.length()) wsmax += 10 + ci.messages.length();
        }
        if(wsmax <= 0)
        {
            reliablemessages = false;
            return false;
        }
        worldstate &ws = allocworldstate();
        ws.setup(2*wsmax);
        int mtu = getservermtu() - 100;
        if(mtu <= 0) mtu = ws.len;
        ucharbuf wsbuf(ws.data, ws.len);
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            if(ci.state.aitype != AI_NONE) continue;
            if(!spaghetti::simplehook(spaghetti::hotstring::worldstate_pos, ci)) addposition(ws, wsbuf, mtu, ci, ci);
            loopvj(ci.bots)
            {
                {
                    auto& ci = clients[i]->bots[j];
                    if(spaghetti::simplehook(spaghetti::hotstring::worldstate_pos, ci)) continue;
                }
                addposition(ws, wsbuf, mtu, *ci.bots[j], ci);
            }
        }
        sendpositions(ws, wsbuf);
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            if(ci.state.aitype != AI_NONE) continue;
            if(!spaghetti::simplehook(spaghetti::hotstring::worldstate_msg, ci)) addmessages(ws, wsbuf, mtu, ci, ci);
            loopvj(ci.bots)
            {
                {
                    auto& ci = clients[i]->bots[j];
                    if(spaghetti::simplehook(spaghetti::hotstring::worldstate_msg, ci)) continue;
                }
                addmessages(ws, wsbuf, mtu, *ci.bots[j], ci);
            }
        }
        sendmessages(ws, wsbuf);
        reliablemessages = false;
        if(ws.uses) return true;
        ws.cleanup();
        return false;
    }

    bool sendpackets(bool force)
    {
        if(clients.empty() || (!hasnonlocalclients() && !demorecord)) return false;
        enet_uint32 curtime = enet_time_get()-lastsend;
        if(curtime<33 && !force) return false;
        bool flush = buildworldstate();
        lastsend += curtime - (curtime%33);
        return flush;
    }

    template<class T>
    void sendstate(gamestate &gs, T &p)
    {
        putint(p, gs.lifesequence);
        putint(p, gs.health);
        putint(p, gs.maxhealth);
        putint(p, gs.armour);
        putint(p, gs.armourtype);
        putint(p, gs.gunselect);
        loopi(GUN_PISTOL-GUN_SG+1) putint(p, gs.ammo[GUN_SG+i]);
    }

    void spawnstate(clientinfo *ci)
    {
        if(spaghetti::simplehook(spaghetti::hotstring::spawnstate, ci)) return;
        gamestate &gs = ci->state;
        gs.spawnstate(gamemode);
        gs.lifesequence = (gs.lifesequence + 1)&0x7F;
    }

    void sendspawn(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        spawnstate(ci);
        sendf(ci->ownernum, 1, "rii7v", N_SPAWNSTATE, ci->clientnum, gs.lifesequence,
            gs.health, gs.maxhealth,
            gs.armour, gs.armourtype,
            gs.gunselect, GUN_PISTOL-GUN_SG+1, &gs.ammo[GUN_SG]);
        gs.lastspawn = gamemillis;
        spaghetti::simplehook(spaghetti::hotstring::spawned, ci);
    }

    void sendwelcome(clientinfo *ci)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        int chan = welcomepacket(p, ci);
        sendpacket(ci->clientnum, chan, p.finalize());
    }

    void putinitclient(clientinfo *ci, packetbuf &p)
    {
        if(ci->state.aitype != AI_NONE)
        {
            putint(p, N_INITAI);
            putint(p, ci->clientnum);
            putint(p, ci->ownernum);
            putint(p, ci->state.aitype);
            putint(p, ci->state.skill);
            putint(p, ci->playermodel);
            sendstring(ci->name, p);
            sendstring(ci->team, p);
        }
        else
        {
            putint(p, N_INITCLIENT);
            putint(p, ci->clientnum);
            sendstring(ci->name, p);
            sendstring(ci->team, p);
            putint(p, ci->playermodel);
        }
    }

    void welcomeinitclient(packetbuf &p, int exclude = -1)
    {
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci->connected || ci->clientnum == exclude) continue;

            putinitclient(ci, p);
        }
    }

    bool hasmap(clientinfo *ci)
    {
        return (m_edit && (clients.length() > 0 || ci->local)) ||
               (smapname[0] && (!m_timed || gamemillis < gamelimit || (ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || numclients(ci->clientnum, true, true, true)));
    }

    int welcomepacket(packetbuf &p, clientinfo *ci)
    {
        putint(p, N_WELCOME);
        putint(p, N_MAPCHANGE);
        sendstring(smapname, p);
        putint(p, gamemode);
        putint(p, notgotitems ? 1 : 0);
        if(!ci || (m_timed && smapname[0]))
        {
            putint(p, N_TIMEUP);
            putint(p, gamemillis < gamelimit && !interm ? max((gamelimit - gamemillis)/1000, 1) : 0);
        }
        if(!notgotitems)
        {
            putint(p, N_ITEMLIST);
            loopv(sents) if(sents[i].spawned)
            {
                putint(p, i);
                putint(p, sents[i].type);
            }
            putint(p, -1);
        }
        bool hasmaster = false;
        if(mastermode != MM_OPEN)
        {
            putint(p, N_CURRENTMASTER);
            putint(p, mastermode);
            hasmaster = true;
        }
        loopv(clients) if(clients[i]->privilege >= PRIV_MASTER)
        {
            if(!hasmaster)
            {
                putint(p, N_CURRENTMASTER);
                putint(p, mastermode);
                hasmaster = true;
            }
            putint(p, clients[i]->clientnum);
            putint(p, clients[i]->privilege);
        }
        if(hasmaster) putint(p, -1);
        if(gamepaused)
        {
            putint(p, N_PAUSEGAME);
            putint(p, 1);
            putint(p, -1);
        }
        if(gamespeed != 100)
        {
            putint(p, N_GAMESPEED);
            putint(p, gamespeed);
            putint(p, -1);
        }
        if(m_teammode)
        {
            putint(p, N_TEAMINFO);
            enumerate(teaminfos, teaminfo, t,
                if(t.frags) { sendstring(t.team, p); putint(p, t.frags); }
            );
            sendstring("", p);
        } 
        if(ci)
        {
            putint(p, N_SETTEAM);
            putint(p, ci->clientnum);
            sendstring(ci->team, p);
            putint(p, -1);
        }
        if(ci && (m_demo || m_mp(gamemode)) && ci->state.state!=CS_SPECTATOR)
        {
            if(smode && !smode->canspawn(ci, true))
            {
                ci->state.state = CS_DEAD;
                putint(p, N_FORCEDEATH);
                putint(p, ci->clientnum);
                sendf(-1, 1, "ri2x", N_FORCEDEATH, ci->clientnum, ci->clientnum);
            }
            else
            {
                gamestate &gs = ci->state;
                spawnstate(ci);
                putint(p, N_SPAWNSTATE);
                putint(p, ci->clientnum);
                sendstate(gs, p);
                gs.lastspawn = gamemillis;
            }
        }
        if(ci && ci->state.state==CS_SPECTATOR)
        {
            putint(p, N_SPECTATOR);
            putint(p, ci->clientnum);
            putint(p, 1);
            sendf(-1, 1, "ri3x", N_SPECTATOR, ci->clientnum, 1, ci->clientnum);
        }
        if(!ci || clients.length()>1)
        {
            putint(p, N_RESUME);
            loopv(clients)
            {
                clientinfo *oi = clients[i];
                if(ci && oi->clientnum==ci->clientnum) continue;
                putint(p, oi->clientnum);
                putint(p, oi->state.state);
                putint(p, oi->state.frags);
                putint(p, oi->state.flags);
                putint(p, oi->state.deaths);
                putint(p, oi->state.quadmillis);
                sendstate(oi->state, p);
            }
            putint(p, -1);
            welcomeinitclient(p, ci ? ci->clientnum : -1);
        }
        if(smode) smode->initclient(ci, p, true);
        return 1;
    }

    bool restorescore(clientinfo *ci)
    {
        //if(ci->local) return false;
        savedscore *sc = findscore(ci, false);
        if(sc)
        {
            if(spaghetti::simplehook(spaghetti::hotstring::restoregamestate, sc, ci)) return true;
            sc->restore(ci->state);
            return true;
        }
        return false;
    }

    void sendresume(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        sendf(-1, 1, "ri3i4i6vi", N_RESUME, ci->clientnum, gs.state,
            gs.frags, gs.flags, gs.deaths, gs.quadmillis,
            gs.lifesequence,
            gs.health, gs.maxhealth,
            gs.armour, gs.armourtype,
            gs.gunselect, GUN_PISTOL-GUN_SG+1, &gs.ammo[GUN_SG], -1);
    }

    void sendinitclient(clientinfo *ci)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putinitclient(ci, p);
        sendpacket(-1, 1, p.finalize(), ci->clientnum);
    }

    void loaditems()
    {
        resetitems();
        notgotitems = true;
        if(spaghetti::simplehook(spaghetti::hotstring::loaditems)) return;
        if(m_edit || !loadents(smapname, ments, &mcrc))
            return;
        loopv(ments) if(canspawnitem(ments[i].type))
        {
            server_entity se = { NOTUSED, 0, false };
            while(sents.length()<=i) sents.add(se);
            sents[i].type = ments[i].type;
            if(m_mp(gamemode) && delayspawn(sents[i].type)) sents[i].spawntime = spawntime(sents[i].type);
            else sents[i].spawned = true;
        }
        notgotitems = false;
    }
        
    void changemap(const char *map, int mode)
    {
        stopdemo();
        pausegame(false);
        changegamespeed(100);
        spaghetti::later::cleargame();
        if(smode) smode->cleanup();
        aiman::clearai();

        gamemode = mode;
        gamemillis = 0;
        gamelimit = 10*60000;
        interm = 0;
        nextexceeded = 0;
        copystring(smapname, map);
        scores.shrink(0);
        shouldcheckteamkills = false;
        teamkills.shrink(0);
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
        }

        if(!m_mp(gamemode)) kicknonlocalclients(DISC_LOCAL);

        sendf(-1, 1, "risii", N_MAPCHANGE, smapname, gamemode, 1);
        loaditems();

        if(m_capture) smode = &capturemode;
        else if(m_ctf) smode = &ctfmode;
        else if(m_collect) smode = &collectmode;
        else smode = NULL;

        clearteaminfo();
        if(m_teammode) autoteam();

        if(m_timed && smapname[0]) sendf(-1, 1, "ri2", N_TIMEUP, gamemillis < gamelimit && !interm ? max((gamelimit - gamemillis)/1000, 1) : 0);
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            ci->mapchange();
            ci->state.lasttimeplayed = lastmillis;
            if(m_mp(gamemode) && ci->state.state!=CS_SPECTATOR) sendspawn(ci);
        }

        aiman::changemap();

        if(m_demo)
        {
            if(clients.length()) setupdemoplayback();
        }
        else
        {
            if(demonextmatch) setupdemorecord();
            demonextmatch = autorecorddemo!=0;
        }

        if(smode)
        {
            if(!spaghetti::simplehook(spaghetti::hotstring::servmodesetup))
                smode->setup();
        }
        spaghetti::simpleconstevent(spaghetti::hotstring::changemap, map, mode);
    }

    void rotatemap(bool next)
    {
        if(!maprotations.inrange(curmaprotation))
        {
            std::string map = "";
            int mode = 1;
            const char *reason = "maprotationfailure";
            if(spaghetti::simplehook(spaghetti::hotstring::prechangemap, map, mode, reason)) return;
            changemap(map.c_str(), mode);
            return;
        }
        if(next) 
        {
            curmaprotation = findmaprotation(gamemode, smapname);
            if(curmaprotation >= 0) nextmaprotation();
            else curmaprotation = smapname[0] ? max(findmaprotation(gamemode, ""), 0) : 0;
        }
        maprotation &rot = maprotations[curmaprotation];
        std::string map = rot.map;
        int mode = rot.findmode(gamemode);
        const char *reason = "maprotation";
        if(spaghetti::simplehook(spaghetti::hotstring::prechangemap, map, mode, reason)) return;
        changemap(map.c_str(), mode);
    }
    
    struct votecount
    {
        char *map;
        int mode, count;
        votecount() {}
        votecount(char *s, int n) : map(s), mode(n), count(0) {}
    };

    void checkvotes(bool force = false)
    {
        vector<votecount> votes;
        int maxvotes = 0;
        loopv(clients)
        {
            clientinfo *oi = clients[i];
            if(oi->state.state==CS_SPECTATOR && !oi->privilege && !oi->local) continue;
            if(oi->state.aitype!=AI_NONE) continue;
            maxvotes++;
            if(!m_valid(oi->modevote)) continue;
            votecount *vc = NULL;
            loopvj(votes) if(!strcmp(oi->mapvote, votes[j].map) && oi->modevote==votes[j].mode)
            {
                vc = &votes[j];
                break;
            }
            if(!vc) vc = &votes.add(votecount(oi->mapvote, oi->modevote));
            vc->count++;
        }
        votecount *best = NULL;
        loopv(votes) if(!best || votes[i].count > best->count || (votes[i].count == best->count && rnd(2))) best = &votes[i];
        if(force || (best && best->count > maxvotes/2))
        {
            sendpackets(true);
            if(demorecord) enddemorecord();
            if(best && (best->count > (force ? 1 : maxvotes/2)))
            {
                std::string map = best->map;
                int mode = best->mode;
                const char *reason = "vote";
                if(spaghetti::simplehook(spaghetti::hotstring::prechangemap, map, mode, reason)) return;
                sendservmsg(force ? "vote passed by default" : "vote passed by majority");
                changemap(map.c_str(), mode);
            }
            else rotatemap(true);
        }
    }

    void forcemap(const char *map, int mode)
    {
        std::string s = map;
        {
            std::string& map = s;
            const char *reason = "force";
            if(spaghetti::simplehook(spaghetti::hotstring::prechangemap, map, mode, reason)) return;
        }
        map = s.c_str();
        stopdemo();
        if(!map[0] && !m_check(mode, M_EDIT)) 
        {
            int idx = findmaprotation(mode, smapname);
            if(idx < 0 && smapname[0]) idx = findmaprotation(mode, "");
            if(idx < 0) return;
            map = maprotations[idx].map;
        }
        if(hasnonlocalclients()) sendservmsgf("local player forced %s on map %s", modename(mode), map[0] ? map : "[new map]");
        changemap(map, mode);
    }

    void vote(const char *map, int reqmode, int sender)
    {
        clientinfo *ci = getinfo(sender);
        if(!ci || (ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || (!ci->local && !m_mp(reqmode))) return;
        if(!m_valid(reqmode)) return;
        if(!map[0] && !m_check(reqmode, M_EDIT)) 
        {
            int idx = findmaprotation(reqmode, smapname);
            if(idx < 0 && smapname[0]) idx = findmaprotation(reqmode, "");
            if(idx < 0) return;
            map = maprotations[idx].map;
        }
        if(lockmaprotation && !ci->local && ci->privilege < (lockmaprotation > 1 ? PRIV_ADMIN : PRIV_MASTER) && findmaprotation(reqmode, map) < 0) 
        {
            sendf(sender, 1, "ris", N_SERVMSG, "This server has locked the map rotation.");
            return;
        }
        copystring(ci->mapvote, map);
        ci->modevote = reqmode;
        if(ci->local || (ci->privilege && mastermode>=MM_VETO))
        {
            std::string map = (const char*)ci->mapvote;
            int mode = ci->modevote;
            const char *reason = "veto";
            if(spaghetti::simplehook(spaghetti::hotstring::prechangemap, map, mode, reason)) return;
            sendpackets(true);
            if(demorecord) enddemorecord();
            if(!ci->local || hasnonlocalclients())
                sendservmsgf("%s forced %s on map %s", colorname(ci), modename(mode), map.c_str()[0] ? map.c_str() : "[new map]");
            changemap(map.c_str(), mode);
        }
        else
        {
            sendservmsgf("%s suggests %s on map %s (select map to vote)", colorname(ci), modename(reqmode), map[0] ? map : "[new map]");
            checkvotes();
        }
    }

    VAR(overtime, 0, 0, 1);

    bool checkovertime()
    {
        if(!m_timed || !overtime) return false;
        const char* topteam = NULL;
        int topscore = INT_MIN;
        bool tied = false;
        if(m_teammode)
        {
            vector<teamscore> scores;
            if(smode && smode->hidefrags()) smode->getteamscores(scores);
            loopv(clients)
            {
                clientinfo *ci = clients[i];
                if(ci->state.state==CS_SPECTATOR || !ci->team[0]) continue;
                int score = 0;
                if(smode && smode->hidefrags())
                {
                    int idx = scores.htfind(ci->team);
                    if(idx >= 0) score = scores[idx].score;
                }
                else if(teaminfo *ti = teaminfos.access(ci->team)) score = ti->frags;
                if(!topteam || score > topscore) { topteam = ci->team; topscore = score; tied = false; }
                else if(score == topscore && strcmp(ci->team, topteam)) tied = true;
            }
        }
        else
        {
            loopv(clients)
            {
                clientinfo *ci = clients[i];
                if(ci->state.state==CS_SPECTATOR) continue;
                int score = ci->state.frags;
                if(score > topscore) { topscore = score; tied = false; }
                else if(score == topscore) tied = true;
            }
        }
        if(!tied) return false;
        if(spaghetti::simplehook(spaghetti::hotstring::preovertime)) return true;
        sendservmsg("the game is tied with overtime");
        gamelimit = max(gamemillis, gamelimit) + 2*60000;
        sendf(-1, 1, "ri2", N_TIMEUP, max((gamelimit - gamemillis)/1000, 1));
        spaghetti::simpleconstevent(spaghetti::hotstring::overtime);
        return true;
    }

    void checkintermission(bool force = false)
    {
        if(gamemillis >= gamelimit && !interm && (force || !checkovertime()))
        {
            if(spaghetti::simplehook(spaghetti::hotstring::preintermission)) return;
            sendf(-1, 1, "ri2", N_TIMEUP, 0);
            if(smode) smode->intermission();
            changegamespeed(100);
            interm = gamemillis + 10000;
            spaghetti::simpleconstevent(spaghetti::hotstring::intermission);
        }
    }

    void startintermission() { gamelimit = min(gamelimit, gamemillis); checkintermission(true); }

    void dodamage(clientinfo *target, clientinfo *actor, int damage, int gun, vec hitpush = vec(0, 0, 0))
    {
        if(!spaghetti::simplehook(spaghetti::hotstring::dodamage, target, actor, damage, gun, hitpush))
            target->state.dodamage(damage);
        gamestate &ts = target->state;
        if(spaghetti::simplehook(spaghetti::hotstring::damageeffects, target, actor, damage, gun, hitpush)) return;
        if(target!=actor && !isteam(target->team, actor->team)) actor->state.damage += damage;
        sendf(-1, 1, "ri6", N_DAMAGE, target->clientnum, actor->clientnum, damage, ts.armour, ts.health);
        if(target==actor) target->setpushed();
        else if(!hitpush.iszero())
        {
            ivec v(vec(hitpush).rescale(DNF));
            sendf(ts.health<=0 ? -1 : target->ownernum, 1, "ri7", N_HITPUSH, target->clientnum, gun, damage, v.x, v.y, v.z);
            target->setpushed();
        }
        if(ts.health<=0)
        {
            target->state.deaths++;
            int fragvalue = smode ? smode->fragvalue(target, actor) : (target==actor || isteam(target->team, actor->team) ? -1 : 1);
            actor->state.frags += fragvalue;
            if(fragvalue>0)
            {
                int friends = 0, enemies = 0; // note: friends also includes the fragger
                if(m_teammode) loopv(clients) if(strcmp(clients[i]->team, actor->team)) enemies++; else friends++;
                else { friends = 1; enemies = clients.length()-1; }
                actor->state.effectiveness += fragvalue*friends/float(max(enemies, 1));
            }
            teaminfo *t = m_teammode ? teaminfos.access(actor->team) : NULL;
            if(t) t->frags += fragvalue; 
            sendf(-1, 1, "ri5", N_DIED, target->clientnum, actor->clientnum, actor->state.frags, t ? t->frags : 0);
            target->position.setsize(0);
            if(smode && !spaghetti::simplehook(spaghetti::hotstring::servmodedied, target, actor)) smode->died(target, actor);
            ts.state = CS_DEAD;
            ts.lastdeath = gamemillis;
            if(actor!=target && isteam(actor->team, target->team)) 
            {
                actor->state.teamkills++;
                addteamkill(actor, target, 1);
            }
            ts.deadflush = ts.lastdeath + DEATHMILLIS;
            // don't issue respawn yet until DEATHMILLIS has elapsed
            // ts.respawn();
        }
        spaghetti::simpleconstevent(spaghetti::hotstring::damaged, target, actor, damage, gun, hitpush);
    }

    void suicide(clientinfo *ci)
    {
        if(spaghetti::simplehook(spaghetti::hotstring::presuicide, ci)) return;
        gamestate &gs = ci->state;
        if(gs.state!=CS_ALIVE) return;
        int fragvalue = smode ? smode->fragvalue(ci, ci) : -1;
        ci->state.frags += fragvalue;
        ci->state.deaths++;
        teaminfo *t = m_teammode ? teaminfos.access(ci->team) : NULL;
        if(t) t->frags += fragvalue;
        sendf(-1, 1, "ri5", N_DIED, ci->clientnum, ci->clientnum, gs.frags, t ? t->frags : 0);
        ci->position.setsize(0);
        auto target = ci;
        if(smode && !spaghetti::simplehook(spaghetti::hotstring::servmodedied, target)) smode->died(ci, NULL);
        gs.state = CS_DEAD;
        gs.lastdeath = gamemillis;
        gs.respawn();
        spaghetti::simpleconstevent(spaghetti::hotstring::suicide, ci);
    }

    void suicideevent::process(clientinfo *ci)
    {
        suicide(ci);
    }

    void explodeevent::process(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        switch(gun)
        {
            case GUN_RL:
                if(!gs.rockets.remove(id)) return;
                break;

            case GUN_GL:
                if(!gs.grenades.remove(id)) return;
                break;

            default:
                return;
        }
        sendf(-1, 1, "ri4x", N_EXPLODEFX, ci->clientnum, gun, id, ci->ownernum);
        bool dohits = true;
        const auto event = this;
        spaghetti::simpleevent(spaghetti::hotstring::explode, event, ci, dohits);
        if(!dohits) return;
        loopv(hits)
        {
            hitinfo &h = hits[i];
            clientinfo *target = getinfo(h.target);
            if(!target || target->state.state!=CS_ALIVE || h.lifesequence!=target->state.lifesequence || h.dist<0 || h.dist>guns[gun].exprad) continue;

            bool dup = false;
            loopj(i) if(hits[j].target==h.target) { dup = true; break; }
            if(dup) continue;

            int damage = guns[gun].damage;
            if(gs.quadmillis) damage *= 4;
            damage = int(damage*(1-h.dist/EXP_DISTSCALE/guns[gun].exprad));
            if(target==ci) damage /= EXP_SELFDAMDIV;
            dodamage(target, ci, damage, gun, h.dir);
        }
    }

    void shotevent::process(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        int wait = millis - gs.lastshot;
        if(!gs.isalive(gamemillis) ||
           wait<gs.gunwait ||
           gun<GUN_FIST || gun>GUN_PISTOL ||
           gs.ammo[gun]<=0 || (guns[gun].range && from.dist(to) > guns[gun].range + 1))
            return;
        if(gun!=GUN_FIST) gs.ammo[gun]--;
        gs.lastshot = millis;
        gs.gunwait = guns[gun].attackdelay;
        sendf(-1, 1, "rii9x", N_SHOTFX, ci->clientnum, gun, id,
                int(from.x*DMF), int(from.y*DMF), int(from.z*DMF),
                int(to.x*DMF), int(to.y*DMF), int(to.z*DMF),
                ci->ownernum);
        gs.shotdamage += guns[gun].damage*(gs.quadmillis ? 4 : 1)*guns[gun].rays;
        bool dohits = false;
        switch(gun)
        {
            case GUN_RL: gs.rockets.add(id); break;
            case GUN_GL: gs.grenades.add(id); break;
            default: dohits = true;
        }
        const auto event = this;
        spaghetti::simpleevent(spaghetti::hotstring::shot, event, ci, dohits);
        if(!dohits) return;
        int totalrays = 0, maxrays = guns[gun].rays;
        loopv(hits)
        {
            hitinfo &h = hits[i];
            clientinfo *target = getinfo(h.target);
            if(!target || target->state.state!=CS_ALIVE || h.lifesequence!=target->state.lifesequence || h.rays<1 || h.dist > guns[gun].range + 1) continue;

            totalrays += h.rays;
            if(totalrays>maxrays) continue;
            int damage = h.rays*guns[gun].damage;
            if(gs.quadmillis) damage *= 4;
            dodamage(target, ci, damage, gun, h.dir);
        }
    }

    void pickupevent::process(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        if(m_mp(gamemode) && !gs.isalive(gamemillis)) return;
        pickup(ent, ci->clientnum);
    }

    bool gameevent::flush(clientinfo *ci, int fmillis)
    {
        process(ci);
        return true;
    }

    bool timedevent::flush(clientinfo *ci, int fmillis)
    {
        if(millis > fmillis) return false;
        else if(millis >= ci->lastevent)
        {
            ci->lastevent = millis;
            process(ci);
        }
        return true;
    }

    void clearevent(clientinfo *ci)
    {
        delete ci->events.remove(0);
    }

    void flushevents(clientinfo *ci, int millis)
    {
        while(ci->events.length())
        {
            gameevent *ev = ci->events[0];
            if(ev->flush(ci, millis)) clearevent(ci);
            else break;
        }
    }

    void processevents()
    {
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(curtime>0 && ci->state.quadmillis) ci->state.quadmillis = max(ci->state.quadmillis-curtime, 0);
            flushevents(ci, gamemillis);
        }
    }

    void cleartimedevents(clientinfo *ci)
    {
        int keep = 0;
        loopv(ci->events)
        {
            if(ci->events[i]->keepable())
            {
                if(keep < i)
                {
                    for(int j = keep; j < i; j++) delete ci->events[j];
                    ci->events.remove(keep, i - keep);
                    i = keep;
                }
                keep = i+1;
                continue;
            }
        }
        while(ci->events.length() > keep) delete ci->events.pop();
        ci->timesync = false;
    }

    void serverupdate()
    {
        if(shouldstep && !gamepaused)
        {
            gamemillis += curtime;
            spaghetti::simpleconstevent(spaghetti::hotstring::worldupdate);
            if(curtime) spaghetti::later::checkgame();

            if(m_demo) readdemo();
            else if(!m_timed || gamemillis < gamelimit)
            {
                processevents();
                if(curtime)
                {
                    loopv(sents) if(sents[i].spawntime) // spawn entities when timer reached
                    {
                        int oldtime = sents[i].spawntime;
                        sents[i].spawntime -= curtime;
                        if(sents[i].spawntime<=0)
                        {
                            auto& ent = sents[i];
                            if(spaghetti::simplehook(spaghetti::hotstring::preitemspawn, ent, i)) continue;
                            sents[i].spawntime = 0;
                            sents[i].spawned = true;
                            sendf(-1, 1, "ri2", N_ITEMSPAWN, i);
                            spaghetti::simpleconstevent(spaghetti::hotstring::itemspawn, ent, i);
                        }
                        else if(sents[i].spawntime<=10000 && oldtime>10000 && (sents[i].type==I_QUAD || sents[i].type==I_BOOST))
                        {
                            auto& ent = sents[i];
                            if(spaghetti::simplehook(spaghetti::hotstring::preannounce, ent, i)) continue;
                            sendf(-1, 1, "ri2", N_ANNOUNCE, ent.type);
                            spaghetti::simpleconstevent(spaghetti::hotstring::announce, ent, i);
                        }
                    }
                }
                aiman::checkai();
                if(smode)
                {
                    if(!spaghetti::simplehook(spaghetti::hotstring::servmodeupdate))
                        smode->update();
                }
            }
        }

        while(bannedips.length() && bannedips[0].expire-totalmillis <= 0) bannedips.remove(0);
        loopv(connects) if(totalmillis-connects[i]->connectmillis>15000)
        {
            auto ci = connects[i];
            if(spaghetti::simplehook(spaghetti::hotstring::jointimeout, ci)) continue;
            disconnect_client(connects[i]->clientnum, DISC_TIMEOUT);
        }

        if(nextexceeded && gamemillis > nextexceeded && (!m_timed || gamemillis < gamelimit))
        {
            nextexceeded = 0;
            loopvrev(clients) 
            {
                clientinfo &c = *clients[i];
                if(c.state.aitype != AI_NONE) continue;
                if(c.checkexceeded() && !spaghetti::simplehook(spaghetti::hotstring::exceeded, c)) disconnect_client(c.clientnum, DISC_MSGERR);
                else c.scheduleexceeded();
            }
        }

        if(shouldcheckteamkills) checkteamkills();

        if(shouldstep && !gamepaused)
        {
            if(m_timed && smapname[0] && gamemillis-curtime>0) checkintermission();
            if(interm > 0 && gamemillis>interm)
            {
                if(demorecord) enddemorecord();
                interm = -1;
                checkvotes(true);
            }
        }

        shouldstep = clients.length() > 0;
    }

    void forcespectator(clientinfo *ci)
    {
        if(ci->state.state==CS_ALIVE) suicide(ci);
        if(smode) smode->leavegame(ci);
        ci->state.state = CS_SPECTATOR;
        ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
        if(!ci->local && (!ci->privilege || ci->warned)) aiman::removeai(ci);
        sendf(-1, 1, "ri3", N_SPECTATOR, ci->clientnum, 1);
        spaghetti::simpleconstevent(spaghetti::hotstring::specstate, ci);
    }

    struct crcinfo
    {
        int crc, matches;

        crcinfo() {}
        crcinfo(int crc, int matches) : crc(crc), matches(matches) {}

        static bool compare(const crcinfo &x, const crcinfo &y) { return x.matches > y.matches; }
    };

    VAR(modifiedmapspectator, 0, 1, 2);

    void checkmaps(int req = -1)
    {
        if(m_edit || !smapname[0]) return;
        vector<crcinfo> crcs;
        int total = 0, unsent = 0, invalid = 0;
        if(mcrc) crcs.add(crcinfo(mcrc, clients.length() + 1));
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE) continue;
            total++;
            if(!ci->clientmap[0])
            {
                if(ci->mapcrc < 0) invalid++;
                else if(!ci->mapcrc) unsent++;
            }
            else
            {
                crcinfo *match = NULL;
                loopvj(crcs) if(crcs[j].crc == ci->mapcrc) { match = &crcs[j]; break; }
                if(!match) crcs.add(crcinfo(ci->mapcrc, 1));
                else match->matches++;
            }
        }
        if(!mcrc && total - unsent < min(total, 4)) return;
        crcs.sort(crcinfo::compare);
        string msg;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE || ci->clientmap[0] || ci->mapcrc >= 0 || (req < 0 && ci->warned)) continue;
            formatstring(msg, "%s has modified map \"%s\"", colorname(ci), smapname);
            sendf(req, 1, "ris", N_SERVMSG, msg);
            if(req < 0) ci->warned = true;
        }
        if(crcs.length() >= 2) loopv(crcs)
        {
            crcinfo &info = crcs[i];
            if(i || info.matches <= crcs[i+1].matches) loopvj(clients)
            {
                clientinfo *ci = clients[j];
                if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE || !ci->clientmap[0] || ci->mapcrc != info.crc || (req < 0 && ci->warned)) continue;
                formatstring(msg, "%s has modified map \"%s\"", colorname(ci), smapname);
                sendf(req, 1, "ris", N_SERVMSG, msg);
                if(req < 0) ci->warned = true;
            }
        }
        if(req < 0 && modifiedmapspectator && (mcrc || modifiedmapspectator > 1)) loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci->local && ci->warned && ci->state.state != CS_SPECTATOR) forcespectator(ci);
        }
    }

    bool shouldspectate(clientinfo *ci)
    {
        return !ci->local && ci->warned && modifiedmapspectator && (mcrc || modifiedmapspectator > 1);
    }

    void unspectate(clientinfo *ci)
    {
        if(shouldspectate(ci)) return;
        ci->state.state = CS_DEAD;
        ci->state.respawn();
        ci->state.lasttimeplayed = lastmillis;
        aiman::addclient(ci);
        sendf(-1, 1, "ri3", N_SPECTATOR, ci->clientnum, 0);
        spaghetti::simpleconstevent(spaghetti::hotstring::specstate, ci);
        if(ci->clientmap[0] || ci->mapcrc) checkmaps();
        if(!hasmap(ci)) rotatemap(true);
    }

    void sendservinfo(clientinfo *ci)
    {
        sendf(ci->clientnum, 1, "ri5ss", N_SERVINFO, ci->clientnum, PROTOCOL_VERSION, ci->sessionid, serverpass[0] ? 1 : 0, (const char*)serverdesc, (const char*)serverauth);
    }

    void noclients()
    {
        bannedips.shrink(0);
        aiman::clearai();
        spaghetti::simpleconstevent(spaghetti::hotstring::noclients);
    }

    void localconnect(int n)
    {
        clientinfo *ci = getinfo(n);
        ci->clientnum = ci->ownernum = n;
        ci->connectmillis = totalmillis;
        ci->sessionid = (rnd(0x1000000)*((totalmillis%10000)+1))&0xFFFFFF;
        ci->local = true;

        connects.add(ci);
        sendservinfo(ci);
    }

    void localdisconnect(int n)
    {
        if(m_demo) enddemoplayback();
        clientdisconnect(n);
    }

    int clientconnect(int n, uint ip)
    {
        clientinfo *ci = getinfo(n);
        ci->clientnum = ci->ownernum = n;
        ci->connectmillis = totalmillis;
        ci->sessionid = (rnd(0x1000000)*((totalmillis%10000)+1))&0xFFFFFF;

        connects.add(ci);
        if(!m_mp(gamemode)) return DISC_LOCAL;
        sendservinfo(ci);
        spaghetti::simpleconstevent(spaghetti::hotstring::clientconnect, ci, ip);
        return DISC_NONE;
    }

    void clientdisconnect(int n, const int reason)
    {
        clientinfo *ci = getinfo(n);
        spaghetti::simpleconstevent(spaghetti::hotstring::clientdisconnect, ci, reason);
        loopv(clients) if(clients[i]->authkickvictim == ci->clientnum) clients[i]->cleanauth(); 
        if(ci->connected)
        {
            if(ci->privilege) setmaster(ci, false);
            if(smode) smode->leavegame(ci, true);
            ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
            savescore(ci);
            sendf(-1, 1, "ri2", N_CDIS, n);
            clients.removeobj(ci);
            aiman::removeai(ci);
            if(!numclients(-1, false, true)) noclients(); // bans clear when server empties
            if(ci->local) checkpausegame();
        }
        else connects.removeobj(ci);
    }

    int reserveclients() { return 3; }

    extern void verifybans();

    struct banlist
    {
        vector<ipmask> bans;

        void clear() { bans.shrink(0); }

        bool check(uint ip)
        {
            loopv(bans) if(bans[i].check(ip)) return true;
            return false;
        }

        void add(const char *ipname)
        {
            ipmask ban;
            ban.parse(ipname);
            bans.add(ban);

            verifybans();
        }
    } ipbans, gbans; 

    bool checkbans(uint ip)
    {
        loopv(bannedips) if(bannedips[i].ip==ip) return true;
        return ipbans.check(ip) || gbans.check(ip);
    }

    void verifybans()
    {
        loopvrev(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->state.aitype != AI_NONE || ci->local || ci->privilege >= PRIV_ADMIN) continue;
            if(checkbans(getclientip(ci->clientnum))) disconnect_client(ci->clientnum, DISC_IPBAN);
        }
    }

    ICOMMAND(clearipbans, "", (), ipbans.clear());
    ICOMMAND(ipban, "s", (const char *ipname), ipbans.add(ipname));
       
    int allowconnect(clientinfo *ci, const char *pwd = "")
    {
        if(ci->local) return DISC_NONE;
        if(!m_mp(gamemode)) return DISC_LOCAL;
        if(serverpass[0])
        {
            if(!checkpassword(ci, serverpass, pwd)) return DISC_PASSWORD;
            return DISC_NONE;
        }
        if(adminpass[0] && checkpassword(ci, adminpass, pwd)) return DISC_NONE;
        if(numclients(-1, false, true)>=maxclients) return DISC_MAXCLIENTS;
        uint ip = getclientip(ci->clientnum);
        if(checkbans(ip)) return DISC_IPBAN;
        if(mastermode>=MM_PRIVATE && allowedips.find(ip)<0) return DISC_PRIVATE;
        return DISC_NONE;
    }

    bool allowbroadcast(int n)
    {
        clientinfo *ci = getinfo(n);
        bool allow = ci && ci->connected;
        spaghetti::simpleevent(spaghetti::hotstring::allowbroadcast, ci, allow);
        return allow;
    }

    clientinfo *findauth(uint id)
    {
        loopv(clients) if(clients[i]->authreq == id) return clients[i];
        loopv(connects) if(connects[i]->authreq == id) return connects[i];
        return NULL;
    }


    void authfailed(clientinfo *ci)
    {
        if(!ci) return;
        ci->cleanauth();
        if(ci->connectauth) disconnect_client(ci->clientnum, ci->connectauth);
    }

    void authfailed(uint id)
    {
        authfailed(findauth(id));
    }

    void authsucceeded(uint id)
    {
        clientinfo *ci = findauth(id);
        if(!ci) return;
        ci->cleanauth(ci->connectauth!=0);
        if(ci->connectauth) connected(ci);
        if(ci->authkickvictim >= 0)
        {
            if(setmaster(ci, true, "", ci->authname, NULL, PRIV_AUTH, false, true))
                trykick(ci, ci->authkickvictim, ci->authkickreason, ci->authname, NULL, PRIV_AUTH);    
            ci->cleanauthkick();
        }
        else setmaster(ci, true, "", ci->authname, NULL, PRIV_AUTH);
    }

    void authchallenged(uint id, const char *val, const char *desc = "")
    {
        clientinfo *ci = findauth(id);
        if(!ci) return;
        sendf(ci->clientnum, 1, "risis", N_AUTHCHAL, desc, id, val);
    }

    uint nextauthreq = 0;

    bool tryauth(clientinfo *ci, const char *user, const char *desc)
    {
        bool result = false;
        if(spaghetti::simplehook(spaghetti::hotstring::tryauth, ci, user, desc, result)) return result;
        ci->cleanauth();
        if(!nextauthreq) nextauthreq = 1;
        ci->authreq = nextauthreq++;
        filtertext(ci->authname, user, false, false, 100);
        copystring(ci->authdesc, desc);
        if(ci->authdesc[0])
        {
            userinfo *u = users.access(userkey(ci->authname, ci->authdesc));
            if(u) 
            {
                uint seed[3] = { ::hthash(serverauth) + detrnd(size_t(ci) + size_t(user) + size_t(desc), 0x10000), uint(totalmillis), randomMT() };
                vector<char> buf;
                ci->authchallenge = genchallenge(u->pubkey, seed, sizeof(seed), buf);
                sendf(ci->clientnum, 1, "risis", N_AUTHCHAL, desc, ci->authreq, buf.getbuf());
            }
            else ci->cleanauth();
        }
        else if(!requestmasterf("reqauth %u %s\n", ci->authreq, (const char*)ci->authname))
        {
            ci->cleanauth();
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, "not connected to authentication server");
        }
        if(ci->authreq) return true;
        if(ci->connectauth) disconnect_client(ci->clientnum, ci->connectauth);
        return false;
    }

    bool answerchallenge(clientinfo *ci, uint id, char *_val, const char *desc)
    {
        bool result = false;
        {
            const char* val = _val;
            if(spaghetti::simplehook(spaghetti::hotstring::answerchallenge, ci, id, val, desc)) return result;
        }
        char* val = _val;
        if(ci->authreq != id || strcmp(ci->authdesc, desc)) 
        {
            ci->cleanauth();
            return !ci->connectauth;
        }
        for(char *s = val; *s; s++)
        {
            if(!isxdigit(*s)) { *s = '\0'; break; }
        }
        if(desc[0])
        {
            if(ci->authchallenge && checkchallenge(val, ci->authchallenge))
            {
                userinfo *u = users.access(userkey(ci->authname, ci->authdesc));
                if(u) 
                {
                    if(ci->connectauth) connected(ci);
                    if(ci->authkickvictim >= 0)
                    {
                        if(setmaster(ci, true, "", ci->authname, ci->authdesc, u->privilege, false, true))
                            trykick(ci, ci->authkickvictim, ci->authkickreason, ci->authname, ci->authdesc, u->privilege);
                    }
                    else setmaster(ci, true, "", ci->authname, ci->authdesc, u->privilege);
                }
            }
            ci->cleanauth(); 
        } 
        else if(!requestmasterf("confauth %u %s\n", id, val))
        {
            ci->cleanauth();
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, "not connected to authentication server");
        }
        return ci->authreq || !ci->connectauth;
    }

    void masterconnected()
    {
        spaghetti::simpleevent(spaghetti::hotstring::masterconnected);
    }

    void masterdisconnected()
    {
        loopvrev(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->authreq) authfailed(ci); 
        }
        spaghetti::simpleevent(spaghetti::hotstring::masterdisconnected);
    }

    void processmasterinput(const char *cmd, int cmdlen, const char *args)
    {
        uint id;
        string val;
        if(sscanf(cmd, "failauth %u", &id) == 1)
            authfailed(id);
        else if(sscanf(cmd, "succauth %u", &id) == 1)
            authsucceeded(id);
        else if(sscanf(cmd, "chalauth %u %255s", &id, val) == 2)
            authchallenged(id, val);
        else if(matchstring(cmd, cmdlen, "cleargbans"))
            gbans.clear();
        else if(sscanf(cmd, "addgban %100s", val) == 1)
            gbans.add(val);
    }

    void receivefile(int sender, uchar *data, int len)
    {
        if(!m_edit || len <= 0 || len > 4*1024*1024) return;
        clientinfo *ci = getinfo(sender);
        if(ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) return;
        if(mapdata) DELETEP(mapdata);
        mapdata = opentempfile("mapdata", "w+b");
        if(!mapdata) { sendf(sender, 1, "ris", N_SERVMSG, "failed to open temporary file for map"); return; }
        mapdata->write(data, len);
        sendservmsgf("[%s sent a map to server, \"/getmap\" to receive it]", colorname(ci));
    }

    void sendclipboard(clientinfo *ci)
    {
        if(!ci->lastclipboard || !ci->clipboard) return;
        bool flushed = false;
        loopv(clients)
        {
            clientinfo &e = *clients[i];
            if(e.clientnum != ci->clientnum && e.needclipboard - ci->lastclipboard >= 0)
            {
                if(!flushed) { flushserver(true); flushed = true; }
                sendpacket(e.clientnum, 1, ci->clipboard);
            }
        }
    }

    void connected(clientinfo *ci)
    {
        if(m_demo) enddemoplayback();

        if(!hasmap(ci)) rotatemap(false);

        shouldstep = true;

        connects.removeobj(ci);
        clients.add(ci);

        ci->connectauth = 0;
        ci->connected = true;
        ci->needclipboard = totalmillis ? totalmillis : 1;
        if(!spaghetti::simplehook(spaghetti::hotstring::joinspecstate, ci) && mastermode>=MM_LOCKED) ci->state.state = CS_SPECTATOR;
        ci->state.lasttimeplayed = lastmillis;

        if(m_teammode && !spaghetti::simplehook(spaghetti::hotstring::autoteam, ci))
        {
            const char *worst = chooseworstteam(NULL, ci);
            copystring(ci->team, worst ? worst : "good", MAXTEAMLEN+1);
        }

        sendwelcome(ci);
        if(restorescore(ci)) sendresume(ci);
        sendinitclient(ci);

        aiman::addclient(ci);

        if(m_demo) setupdemoplayback();

        if(servermotd[0]) sendf(ci->clientnum, 1, "ris", N_SERVMSG, (const char*)servermotd);
        spaghetti::simpleconstevent(spaghetti::hotstring::connected, ci);
        if(ci->state.state != CS_SPECTATOR) spaghetti::simpleconstevent(spaghetti::hotstring::spawned, ci);
    }

    void parsepacket(int sender, int chan, packetbuf &p)     // has to parse exactly each byte of the packet
    {
        if(sender<0 || p.packet->flags&ENET_PACKET_FLAG_UNSEQUENCED || chan > 2){
            spaghetti::simpleconstevent(spaghetti::hotstring::martian_transport, sender, chan, p);
            return;
        }
        lua_array<char, MAXTRANS> text;
        int _type, realtype;
        const int& type = _type;
        clientinfo *ci = sender>=0 ? getinfo(sender) : NULL, *cq = ci, *cm = ci;
        if(ci && !ci->connected)
        {
            int curmsg;
            if(chan==0){
                spaghetti::simpleconstevent(spaghetti::hotstring::martian_preconnectchan, sender, p, ci, cq, cm);
                return;
            }
            else if(chan!=1) {
                if(!spaghetti::simplehook(spaghetti::hotstring::martian_preconnectchan, sender, p, ci, cq, cm))
                    disconnect_client(sender, DISC_MSGERR);
                return;
            }
            else while((curmsg = p.length()) < p.maxlen) switch(checktype(realtype = getint(p), ci))
            {
                case N_CONNECT:
                {
                    getstring(text, p);
                    int playermodel = getint(p);
                    lua_string password, authdesc, authname;
                    getstring(password, p, sizeof(password));
                    getstring(authdesc, p, sizeof(authdesc));
                    getstring(authname, p, sizeof(authname));
                    if(spaghetti::simplehook(N_CONNECT, sender, p, curmsg, ci, cq, cm, text, playermodel, password, authdesc, authname)) break;

                    filtertext(text, text, false, false, MAXNAMELEN);
                    if(!text[0]) copystring(text, "unnamed");
                    copystring(ci->name, text, MAXNAMELEN+1);
                    ci->playermodel = playermodel;
                    int disc = allowconnect(ci, password);
                    if(disc)
                    {
                        if(disc == DISC_LOCAL || !serverauth[0] || strcmp(serverauth, authdesc) || !tryauth(ci, authname, authdesc))
                        {
                            disconnect_client(sender, disc);
                            return;
                        }
                        ci->connectauth = disc;
                    }
                    else connected(ci);
                    break;
                }

                case N_AUTHANS:
                {
                    lua_string desc, ans;
                    getstring(desc, p, sizeof(desc));
                    uint id = (uint)getint(p);
                    getstring(ans, p, sizeof(ans));
                    if(spaghetti::simplehook(N_AUTHANS, sender, p, curmsg, ci, cq, cm, desc, ans, id)) break;
                    if(!answerchallenge(ci, id, ans, desc)) 
                    {
                        disconnect_client(sender, ci->connectauth);
                        return;
                    }
                    break;
                }

                case N_PING:
                {
                    int ping = getint(p);
                    spaghetti::simpleevent(N_PING, sender, p, curmsg, ci, cq, cm, ping);
                    break;
                }

                default:
                    _type = realtype;
                    if(!spaghetti::simplehook(spaghetti::hotstring::martian, sender, p, curmsg, ci, cq, cm, type))
                    {
                        disconnect_client(sender, DISC_MSGERR);
                        return;
                    }
                    else break;
            }
            return;
        }
        else if(chan==2)
        {
            if(!spaghetti::simplehook(spaghetti::hotstring::receivefile, sender, p, ci, cq, cm))
                receivefile(sender, p.buf, p.maxlen);
            return;
        }

        if(p.packet->flags&ENET_PACKET_FLAG_RELIABLE) reliablemessages = true;
        #define QUEUE_AI clientinfo *cm = cq;
        #define QUEUE_MSG { if(cm && (!cm->local || demorecord || hasnonlocalclients())) while(curmsg<p.length()) cm->messages.add(p.buf[curmsg++]); }
        #define QUEUE_BUF(body) { \
            if(cm && (!cm->local || demorecord || hasnonlocalclients())) \
            { \
                curmsg = p.length(); \
                { body; } \
            } \
        }
        #define QUEUE_INT(n) QUEUE_BUF(putint(cm->messages, n))
        #define QUEUE_UINT(n) QUEUE_BUF(putuint(cm->messages, n))
        #define QUEUE_STR(text) QUEUE_BUF(sendstring(text, cm->messages))
        int curmsg;
        while((curmsg = p.length()) < p.maxlen) switch(_type = checktype(realtype = getint(p), ci))
        {
            case N_POS:
            {
                int pcn = getuint(p); 
                int physstate = p.get();
                uint flags = getuint(p);
                clientinfo *cp = getinfo(pcn);
                if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
                vec pos, falling;
                float yaw, pitch, roll;
                loopk(3)
                {
                    int n = p.get(); n |= p.get()<<8; if(flags&(1<<k)) { n |= p.get()<<16; if(n&0x800000) n |= ~0U<<24; }
                    pos[k] = n/DMF;
                }
                int dir = p.get(); dir |= p.get()<<8;
                yaw = dir%360;
                pitch = clamp(dir/360, 0, 180)-90;
                roll = clamp(int(p.get()), 0, 180)-90;
                int mag = p.get(); if(flags&(1<<3)) mag |= p.get()<<8;
                dir = p.get(); dir |= p.get()<<8;
                vec vel = vec((dir%360)*RAD, (clamp(dir/360, 0, 180)-90)*RAD).mul(mag/DVELF);
                if(flags&(1<<4))
                {
                    mag = p.get(); if(flags&(1<<5)) mag |= p.get()<<8;
                    if(flags&(1<<6))
                    {
                        dir = p.get(); dir |= p.get()<<8;
                        vecfromyawpitch(dir%360, clamp(dir/360, 0, 180)-90, 1, 0, falling);
                    }
                    else falling = vec(0, 0, -1);
                    falling.mul(mag/DVELF);
                }
                else falling = vec(0, 0, 0);
                if(spaghetti::simplehook(N_POS, sender, p, curmsg, ci, cq, cm, pcn, physstate, flags, cp, pos, yaw, pitch, roll, vel, falling)) break;
                if(cp)
                {
                    if((!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
                    {
                        if(!ci->local && !m_edit && max(vel.magnitude2(), (float)fabs(vel.z)) >= 180)
                            cp->setexceeded();
                        cp->position.setsize(0);
                        while(curmsg<p.length()) cp->position.add(p.buf[curmsg++]);
                    }
                    if(smode && cp->state.state==CS_ALIVE) smode->moved(cp, cp->state.o, cp->gameclip, pos, (flags&0x80)!=0);
                    cp->state.o = pos;
                    cp->gameclip = (flags&0x80)!=0;
                }
                break;
            }

            case N_TELEPORT:
            {
                int pcn = getint(p), teleport = getint(p), teledest = getint(p);
                clientinfo *cp = getinfo(pcn);
                if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
                if(spaghetti::simplehook(N_TELEPORT, sender, p, curmsg, ci, cq, cm, pcn, teleport, teledest, cp)) break;
                if(cp && (!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
                {
                    flushclientposition(*cp);
                    sendf(-1, 0, "ri4x", N_TELEPORT, pcn, teleport, teledest, cp->ownernum); 
                }
                break;
            }

            case N_JUMPPAD:
            {
                int pcn = getint(p), jumppad = getint(p);
                clientinfo *cp = getinfo(pcn);
                if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
                if(spaghetti::simplehook(N_JUMPPAD, sender, p, curmsg, ci, cq, cm, pcn, jumppad, cp)) break;
                if(cp && (!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
                {
                    cp->setpushed();
                    flushclientposition(*cp);
                    sendf(-1, 0, "ri3x", N_JUMPPAD, pcn, jumppad, cp->ownernum);
                }
                break;
            }
                
            case N_FROMAI:
            {
                int qcn = getint(p);
                if(spaghetti::simplehook(N_FROMAI, sender, p, curmsg, ci, cq, cm, qcn)) break;
                if(qcn < 0) cq = ci;
                else
                {
                    cq = getinfo(qcn);
                    if(cq && qcn != sender && cq->ownernum != sender) cq = NULL;
                }
                break;
            }

            case N_EDITMODE:
            {
                int val = getint(p);
                if(spaghetti::simplehook(N_EDITMODE, sender, p, curmsg, ci, cq, cm, val)) break;
                if(!ci->local && !m_edit) break;
                if(val ? ci->state.state!=CS_ALIVE && ci->state.state!=CS_DEAD : ci->state.state!=CS_EDITING) break;
                if(smode)
                {
                    if(val) smode->leavegame(ci);
                    else smode->entergame(ci);
                }
                if(val)
                {
                    ci->state.editstate = ci->state.state;
                    ci->state.state = CS_EDITING;
                    ci->events.setsize(0);
                    ci->state.rockets.reset();
                    ci->state.grenades.reset();
                }
                else ci->state.state = ci->state.editstate;
                QUEUE_MSG;
                break;
            }

            case N_MAPCRC:
            {
                getstring(text, p);
                int crc = getint(p);
                if(spaghetti::simplehook(N_MAPCRC, sender, p, curmsg, ci, cq, cm, text, crc)) break;
                if(!ci) break;
                if(strcmp(text, smapname))
                {
                    if(ci->clientmap[0])
                    {
                        ci->clientmap[0] = '\0';
                        ci->mapcrc = 0;
                    }
                    else if(ci->mapcrc > 0) ci->mapcrc = 0;
                    break;
                }
                copystring(ci->clientmap, text);
                ci->mapcrc = text[0] ? crc : 1;
                checkmaps();
                if(cq && cq != ci && cq->ownernum != ci->clientnum) cq = NULL;
                break;
            }

            case N_CHECKMAPS:
                if(spaghetti::simplehook(N_CHECKMAPS, sender, p, curmsg, ci, cq, cm)) break;
                checkmaps(sender);
                break;

            case N_TRYSPAWN:
                if(spaghetti::simplehook(N_TRYSPAWN, sender, p, curmsg, ci, cq, cm)) break;
                if(!ci || !cq || cq->state.state!=CS_DEAD || cq->state.lastspawn>=0 || (smode && !smode->canspawn(cq))) break;
                if(!ci->clientmap[0] && !ci->mapcrc)
                {
                    ci->mapcrc = -1;
                    checkmaps();
                    if(ci == cq) { if(ci->state.state != CS_DEAD) break; }
                    else if(cq->ownernum != ci->clientnum) { cq = NULL; break; }
                }
                if(cq->state.deadflush)
                {
                    flushevents(cq, cq->state.deadflush);
                    cq->state.respawn();
                }
                cleartimedevents(cq);
                sendspawn(cq);
                break;

            case N_GUNSELECT:
            {
                int gunselect = getint(p);
                if(spaghetti::simplehook(N_GUNSELECT, sender, p, curmsg, ci, cq, cm, gunselect)) break;
                if(!cq || cq->state.state!=CS_ALIVE) break;
                cq->state.gunselect = gunselect >= GUN_FIST && gunselect <= GUN_PISTOL ? gunselect : GUN_FIST;
                QUEUE_AI;
                QUEUE_MSG;
                break;
            }

            case N_SPAWN:
            {
                int ls = getint(p), gunselect = getint(p);
                if(spaghetti::simplehook(N_SPAWN, sender, p, curmsg, ci, cq, cm, ls, gunselect)) break;
                if(!cq || (cq->state.state!=CS_ALIVE && cq->state.state!=CS_DEAD && cq->state.state!=CS_EDITING) || ls!=cq->state.lifesequence || cq->state.lastspawn<0) break;
                cq->state.lastspawn = -1;
                cq->state.state = CS_ALIVE;
                cq->state.gunselect = gunselect >= GUN_FIST && gunselect <= GUN_PISTOL ? gunselect : GUN_FIST;
                cq->exceeded = 0;
                if(smode) smode->spawned(cq);
                QUEUE_AI;
                QUEUE_BUF({
                    putint(cm->messages, N_SPAWN);
                    sendstate(cq->state, cm->messages);
                });
                break;
            }

            case N_SUICIDE:
            {
                if(spaghetti::simplehook(N_SUICIDE, sender, p, curmsg, ci, cq, cm)) break;
                if(cq) cq->addevent(new suicideevent);
                break;
            }

            case N_SHOOT:
            {
                shotevent * const shot = new shotevent;
                shot->id = getint(p);
                shot->millis = cq ? cq->geteventmillis(gamemillis, shot->id) : 0;
                shot->gun = getint(p);
                loopk(3) shot->from[k] = getint(p)/DMF;
                loopk(3) shot->to[k] = getint(p)/DMF;
                int hits = getint(p);
                loopk(hits)
                {
                    if(p.overread()) break;
                    hitinfo &hit = shot->hits.add();
                    hit.target = getint(p);
                    hit.lifesequence = getint(p);
                    hit.dist = getint(p)/DMF;
                    hit.rays = getint(p);
                    loopk(3) hit.dir[k] = getint(p)/DNF;
                }
                if(spaghetti::simplehook(N_SHOOT, sender, p, curmsg, ci, cq, cm, shot))
                {
                    delete shot;
                    break;
                }
                if(cq)
                {
                    cq->addevent(shot);
                    cq->setpushed();
                }
                else delete shot;
                break;
            }

            case N_EXPLODE:
            {
                explodeevent * const exp = new explodeevent;
                int cmillis = getint(p);
                exp->millis = cq ? cq->geteventmillis(gamemillis, cmillis) : 0;
                exp->gun = getint(p);
                exp->id = getint(p);
                int hits = getint(p);
                loopk(hits)
                {
                    if(p.overread()) break;
                    hitinfo &hit = exp->hits.add();
                    hit.target = getint(p);
                    hit.lifesequence = getint(p);
                    hit.dist = getint(p)/DMF;
                    hit.rays = getint(p);
                    loopk(3) hit.dir[k] = getint(p)/DNF;
                }
                if(spaghetti::simplehook(N_EXPLODE, sender, p, curmsg, ci, cq, cm, exp, cmillis))
                {
                    delete exp;
                    break;
                }
                if(cq) cq->addevent(exp);
                else delete exp;
                break;
            }

            case N_ITEMPICKUP:
            {
                int n = getint(p);
                if(spaghetti::simplehook(N_ITEMPICKUP, sender, p, curmsg, ci, cq, cm, n)) break;
                if(!cq) break;
                pickupevent *pickup = new pickupevent;
                pickup->ent = n;
                cq->addevent(pickup);
                break;
            }

            case N_TEXT:
            {
                getstring(text, p);
                if(spaghetti::simplehook(N_TEXT, sender, p, curmsg, ci, cq, cm, text)) break;
                QUEUE_AI;
                QUEUE_INT(N_TEXT);
                filtertext(text, text, true, true);
                QUEUE_STR(text);
                if(isdedicatedserver() && cq) logoutf("%s: %s", colorname(cq), (const char*)text);
                break;
            }

            case N_SAYTEAM:
            {
                getstring(text, p);
                if(spaghetti::simplehook(N_SAYTEAM, sender, p, curmsg, ci, cq, cm, text)) break;
                if(!ci || !cq || (ci->state.state==CS_SPECTATOR && !ci->local && !ci->privilege) || !m_teammode || !cq->team[0]) break;
                filtertext(text, text, true, true);
                loopv(clients)
                {
                    clientinfo *t = clients[i];
                    if(t==cq || t->state.state==CS_SPECTATOR || t->state.aitype != AI_NONE || strcmp(cq->team, t->team)) continue;
                    sendf(t->clientnum, 1, "riis", N_SAYTEAM, cq->clientnum, (const char*)text);
                }
                if(isdedicatedserver() && cq) logoutf("%s <%s>: %s", colorname(cq), (const char*)cq->team, (const char*)text);
                break;
            }

            case N_SWITCHNAME:
            {
                getstring(text, p);
                if(spaghetti::simplehook(N_SWITCHNAME, sender, p, curmsg, ci, cq, cm, text)) break;
                QUEUE_INT(N_SWITCHNAME);
                filtertext(ci->name, text, false, false, MAXNAMELEN);
                if(!ci->name[0]) copystring(ci->name, "unnamed");
                QUEUE_STR(ci->name);
                break;
            }

            case N_SWITCHMODEL:
            {
                int playermodel = getint(p);
                if(spaghetti::simplehook(N_SWITCHMODEL, sender, p, curmsg, ci, cq, cm, playermodel)) break;
                ci->playermodel = playermodel;
                QUEUE_MSG;
                break;
            }

            case N_SWITCHTEAM:
            {
                getstring(text, p);
                if(spaghetti::simplehook(N_SWITCHTEAM, sender, p, curmsg, ci, cq, cm, text)) break;
                filtertext(text, text, false, false, MAXTEAMLEN);
                if(m_teammode && text[0] && strcmp(ci->team, text) && (!smode || smode->canchangeteam(ci, ci->team, text)) && addteaminfo(text))
                {
                    if(ci->state.state==CS_ALIVE) suicide(ci);
                    copystring(ci->team, text);
                    aiman::changeteam(ci);
                    sendf(-1, 1, "riisi", N_SETTEAM, sender, (const char*)ci->team, ci->state.state==CS_SPECTATOR ? -1 : 0);
                }
                break;
            }

            case N_MAPVOTE:
            {
                getstring(text, p);
                int reqmode = getint(p);
                if(spaghetti::simplehook(N_MAPVOTE, sender, p, curmsg, ci, cq, cm, text, reqmode)) break;
                filtertext(text, text, false);
                fixmapname(text);
                vote(text, reqmode, sender);
                break;
            }

            case N_ITEMLIST:
            {
                vector<server_entity> parsesents;
                int n;
                while((n = getint(p))>=0 && n<MAXENTS && !p.overread())
                {
                    server_entity se = { NOTUSED, 0, false };
                    while(parsesents.length()<=n) parsesents.add(se);
                    parsesents[n].type = getint(p);
                    if(canspawnitem(parsesents[n].type))
                    {
                        if(m_mp(gamemode) && delayspawn(parsesents[n].type)) parsesents[n].spawntime = spawntime(parsesents[n].type);
                        else parsesents[n].spawned = true;
                    }
                }
                if(spaghetti::simplehook(N_ITEMLIST, sender, p, curmsg, ci, cq, cm, parsesents)) break;
                if((ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || !notgotitems || strcmp(ci->clientmap, smapname)) break;
                sents = parsesents;
                notgotitems = false;
                break;
            }

            case N_EDITENT:
            {
                int i = getint(p);
                entity ent;
                loopk(3) ent.o[k] = getint(p)/DMF;
                int type = ent.type = getint(p);
                ent.attr1 = getint(p), ent.attr2 = getint(p), ent.attr3 = getint(p), ent.attr4 = getint(p), ent.attr5 = getint(p);
                if(spaghetti::simplehook(N_EDITENT, sender, p, curmsg, ci, cq, cm, i, ent)) break;
                if(!ci || ci->state.state==CS_SPECTATOR) break;
                QUEUE_MSG;
                bool canspawn = canspawnitem(type);
                if(i<MAXENTS && (sents.inrange(i) || canspawnitem(type)))
                {
                    server_entity se = { NOTUSED, 0, false };
                    while(sents.length()<=i) sents.add(se);
                    sents[i].type = type;
                    if(canspawn ? !sents[i].spawned : (sents[i].spawned || sents[i].spawntime))
                    {
                        sents[i].spawntime = canspawn ? 1 : 0;
                        sents[i].spawned = false;
                    }
                }
                break;
            }

            case N_EDITVAR:
            {
                int type = getint(p);
                getstring(text, p);
                double numval = 0;
                lua_string stringval;
                stringval[0] = 0;
                switch(type)
                {
                    case ID_VAR: numval = getint(p); break;
                    case ID_FVAR: numval = getfloat(p); break;
                    case ID_SVAR: getstring(stringval, p);
                }
                if(spaghetti::simplehook(N_EDITVAR, sender, p, curmsg, ci, cq, cm, type, text, numval, stringval)) break;
                if(ci && ci->state.state!=CS_SPECTATOR) QUEUE_MSG;
                break;
            }

            case N_PING:
            {
                int ping = getint(p);
                if(spaghetti::simplehook(N_PING, sender, p, curmsg, ci, cq, cm, ping)) break;
                sendf(sender, 1, "i2", N_PONG, ping);
                break;
            }

            case N_CLIENTPING:
            {
                int ping = getint(p);
                if(spaghetti::simplehook(N_CLIENTPING, sender, p, curmsg, ci, cq, cm, ping)) break;
                if(ci)
                {
                    ci->ping = ping;
                    loopv(ci->bots) ci->bots[i]->ping = ping;
                }
                QUEUE_MSG;
                break;
            }

            case N_MASTERMODE:
            {
                int mm = getint(p);
                if(spaghetti::simplehook(N_MASTERMODE, sender, p, curmsg, ci, cq, cm, mm)) break;
                if((ci->privilege || ci->local) && mm>=MM_OPEN && mm<=MM_PRIVATE && mastermode != mm)
                {
                    if((ci->privilege>=PRIV_ADMIN || ci->local) || (mastermask&(1<<mm)))
                    {
                        mastermode = mm;
                        allowedips.shrink(0);
                        if(mm>=MM_PRIVATE)
                        {
                            loopv(clients) allowedips.add(getclientip(clients[i]->clientnum));
                        }
                        sendf(-1, 1, "rii", N_MASTERMODE, mastermode);
                        spaghetti::simpleconstevent(spaghetti::hotstring::mastermode, ci);
                        //sendservmsgf("mastermode is now %s (%d)", mastermodename(mastermode), mastermode);
                    }
                    else
                    {
                        defformatstring(s, "mastermode %d is disabled on this server", mm);
                        sendf(sender, 1, "ris", N_SERVMSG, s);
                    }
                }
                break;
            }

            case N_CLEARBANS:
            {
                if(spaghetti::simplehook(N_CLEARBANS, sender, p, curmsg, ci, cq, cm)) break;
                if(ci->privilege || ci->local)
                {
                    bannedips.shrink(0);
                    sendservmsg("cleared all bans");
                }
                break;
            }

            case N_KICK:
            {
                int victim = getint(p);
                getstring(text, p);
                if(spaghetti::simplehook(N_KICK, sender, p, curmsg, ci, cq, cm, victim)) break;
                filtertext(text, text);
                trykick(ci, victim, text);
                break;
            }

            case N_SPECTATOR:
            {
                int spectator = getint(p), val = getint(p);
                clientinfo *spinfo = (clientinfo *)getclientinfo(spectator); // no bots
                if(spaghetti::simplehook(N_SPECTATOR, sender, p, curmsg, ci, cq, cm, spectator, val, spinfo)) break;
                if(!ci->privilege && !ci->local && (spectator!=sender || (ci->state.state==CS_SPECTATOR && mastermode>=MM_LOCKED))) break;
                if(!spinfo || !spinfo->connected || (spinfo->state.state==CS_SPECTATOR ? val : !val)) break;

                if(spinfo->state.state!=CS_SPECTATOR && val) forcespectator(spinfo);
                else if(spinfo->state.state==CS_SPECTATOR && !val) unspectate(spinfo);

                if(cq && cq != ci && cq->ownernum != ci->clientnum) cq = NULL;
                break;
            }

            case N_SETTEAM:
            {
                int who = getint(p);
                getstring(text, p);
                clientinfo *wi = getinfo(who);
                if(spaghetti::simplehook(N_SETTEAM, sender, p, curmsg, ci, cq, cm, who, text, wi)) break;
                filtertext(text, text, false, false, MAXTEAMLEN);
                if(!ci->privilege && !ci->local) break;
                if(!m_teammode || !text[0] || !wi || !wi->connected || !strcmp(wi->team, text)) break;
                if((!smode || smode->canchangeteam(wi, wi->team, text)) && addteaminfo(text))
                {
                    if(wi->state.state==CS_ALIVE) suicide(wi);
                    copystring(wi->team, text, MAXTEAMLEN+1);
                }
                aiman::changeteam(wi);
                sendf(-1, 1, "riisi", N_SETTEAM, who, (const char*)wi->team, 1);
                break;
            }

            case N_FORCEINTERMISSION:
                if(spaghetti::simplehook(N_FORCEINTERMISSION, sender, p, curmsg, ci, cq, cm)) break;
                if(ci->local && !hasnonlocalclients()) startintermission();
                break;

            case N_RECORDDEMO:
            {
                int val = getint(p);
                if(spaghetti::simplehook(N_RECORDDEMO, sender, p, curmsg, ci, cq, cm, val)) break;
                if(ci->privilege < (restrictdemos ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                if(!maxdemos || !maxdemosize) 
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "the server has disabled demo recording");
                    break;
                }
                demonextmatch = val!=0;
                sendservmsgf("demo recording is %s for next match", demonextmatch ? "enabled" : "disabled");
                break;
            }

            case N_STOPDEMO:
            {
                if(spaghetti::simplehook(N_STOPDEMO, sender, p, curmsg, ci, cq, cm)) break;
                if(ci->privilege < (restrictdemos ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                stopdemo();
                break;
            }

            case N_CLEARDEMOS:
            {
                int demo = getint(p);
                if(spaghetti::simplehook(N_CLEARDEMOS, sender, p, curmsg, ci, cq, cm, demo)) break;
                if(ci->privilege < (restrictdemos ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                cleardemos(demo);
                break;
            }

            case N_LISTDEMOS:
                if(spaghetti::simplehook(N_LISTDEMOS, sender, p, curmsg, ci, cq, cm)) break;
                if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
                listdemos(sender);
                break;

            case N_GETDEMO:
            {
                int n = getint(p), tag = getint(p);
                if(spaghetti::simplehook(N_GETDEMO, sender, p, curmsg, ci, cq, cm, n, tag)) break;
                if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
                senddemo(ci, n, tag);
                break;
            }

            case N_GETMAP:
                if(spaghetti::simplehook(N_GETMAP, sender, p, curmsg, ci, cq, cm)) break;
                if(!mapdata) sendf(sender, 1, "ris", N_SERVMSG, "no map to send");
                else if(ci->getmap) sendf(sender, 1, "ris", N_SERVMSG, "already sending map");
                else
                {
                    sendservmsgf("[%s is getting the map]", colorname(ci));
                    if((ci->getmap = sendfile(sender, 2, mapdata, "ri", N_SENDMAP)))
                        ci->getmap->freeCallback = freegetmap;
                    ci->needclipboard = totalmillis ? totalmillis : 1;
                }
                break;

            case N_NEWMAP:
            {
                int size = getint(p);
                if(spaghetti::simplehook(N_NEWMAP, sender, p, curmsg, ci, cq, cm, size)) break;
                if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
                if(size>=0)
                {
                    smapname[0] = '\0';
                    resetitems();
                    notgotitems = false;
                    if(smode) smode->newmap();
                }
                QUEUE_MSG;
                break;
            }

            case N_SETMASTER:
            {
                int mn = getint(p), val = getint(p);
                getstring(text, p);
                clientinfo *minfo = (clientinfo *)getclientinfo(mn);
                if(spaghetti::simplehook(N_SETMASTER, sender, p, curmsg, ci, cq, cm, mn, val, text, minfo)) break;
                if(mn != ci->clientnum)
                {
                    if(!ci->privilege && !ci->local) break;
                    if(!minfo || !minfo->connected || (!ci->local && minfo->privilege >= ci->privilege) || (val && minfo->privilege)) break;
                    setmaster(minfo, val!=0, "", NULL, NULL, PRIV_MASTER, true);
                }
                else setmaster(ci, val!=0, text);
                // don't broadcast the master password
                break;
            }

            case N_ADDBOT:
            {
                int skill = getint(p);
                if(spaghetti::simplehook(N_ADDBOT, sender, p, curmsg, ci, cq, cm, skill)) break;
                aiman::reqadd(ci, skill);
                break;
            }

            case N_DELBOT:
            {
                if(spaghetti::simplehook(N_DELBOT, sender, p, curmsg, ci, cq, cm)) break;
                aiman::reqdel(ci);
                break;
            }

            case N_BOTLIMIT:
            {
                int limit = getint(p);
                if(spaghetti::simplehook(N_BOTLIMIT, sender, p, curmsg, ci, cq, cm, limit)) break;
                if(ci) aiman::setbotlimit(ci, limit);
                break;
            }

            case N_BOTBALANCE:
            {
                int balance = getint(p);
                if(spaghetti::simplehook(N_BOTBALANCE, sender, p, curmsg, ci, cq, cm, balance)) break;
                if(ci) aiman::setbotbalance(ci, balance!=0);
                break;
            }

            case N_AUTHTRY:
            {
                lua_string desc, name;
                getstring(desc, p, sizeof(desc));
                getstring(name, p, sizeof(name));
                if(spaghetti::simplehook(N_AUTHTRY, sender, p, curmsg, ci, cq, cm, desc, name)) break;
                tryauth(ci, name, desc);
                break;
            }

            case N_AUTHKICK:
            {
                lua_string desc, name;
                getstring(desc, p, sizeof(desc));
                getstring(name, p, sizeof(name));
                int victim = getint(p);
                getstring(text, p);
                int authpriv = PRIV_AUTH;
                if(spaghetti::simplehook(N_AUTHKICK, sender, p, curmsg, ci, cq, cm, desc, name, victim, authpriv)) break;
                filtertext(text, text);
                if(desc[0])
                {
                    userinfo *u = users.access(userkey(name, desc));
                    if(u) authpriv = u->privilege; else break;
                }
                if(ci->local || ci->privilege >= authpriv) trykick(ci, victim, text);
                else if(trykick(ci, victim, text, name, desc, authpriv, true) && tryauth(ci, name, desc))
                {
                    ci->authkickvictim = victim;
                    ci->authkickreason = newstring(text);
                } 
                break;
            }

            case N_AUTHANS:
            {
                lua_string desc, ans;
                getstring(desc, p, sizeof(desc));
                uint id = (uint)getint(p);
                getstring(ans, p, sizeof(ans));
                if(spaghetti::simplehook(N_AUTHANS, sender, p, curmsg, ci, cq, cm, desc, ans, id)) break;
                answerchallenge(ci, id, ans, desc);
                break;
            }

            case N_PAUSEGAME:
            {
                int val = getint(p);
                if(spaghetti::simplehook(N_PAUSEGAME, sender, p, curmsg, ci, cq, cm, val)) break;
                if(ci->privilege < (restrictpausegame ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                pausegame(val > 0, ci);
                break;
            }

            case N_GAMESPEED:
            {
                int val = getint(p);
                if(spaghetti::simplehook(N_GAMESPEED, sender, p, curmsg, ci, cq, cm, val)) break;
                if(ci->privilege < (restrictgamespeed ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                changegamespeed(val, ci);
                break;
            }

            case N_EDITF:
            case N_EDITM:
            case N_FLIP:
            case N_COPY:
            case N_PASTE:
            case N_ROTATE:
            case N_DELCUBE:
            case N_EDITT:
            case N_REPLACE:
            case N_EDITVSLOT:
            {
                selinfo sel;
                sel.o.x = getint(p); sel.o.y = getint(p); sel.o.z = getint(p);
                sel.s.x = getint(p); sel.s.y = getint(p); sel.s.z = getint(p);
                sel.grid = getint(p); sel.orient = getint(p);
                sel.cx = getint(p); sel.cxs = getint(p); sel.cy = getint(p), sel.cys = getint(p);
                sel.corner = getint(p);
                int delta, dir, mode, tex, newtex, mat, filter, allfaces, insel;
                bool skip = false;
                switch(type)
                {
                    case N_EDITF: dir = getint(p); mode = getint(p); skip = spaghetti::simplehook(N_EDITF, sender, p, curmsg, ci, cq, cm, sel, dir, mode); break;
                    case N_EDITM: mat = getint(p); filter = getint(p); skip = spaghetti::simplehook(N_EDITM, sender, p, curmsg, ci, cq, cm, sel, mat, filter); break;
                    case N_FLIP: skip = spaghetti::simplehook(N_FLIP, sender, p, curmsg, ci, cq, cm, sel); break;
                    case N_COPY: skip = spaghetti::simplehook(N_COPY, sender, p, curmsg, ci, cq, cm, sel); break;
                    case N_PASTE: skip = spaghetti::simplehook(N_PASTE, sender, p, curmsg, ci, cq, cm, sel); break;
                    case N_ROTATE: dir = getint(p); skip = spaghetti::simplehook(N_ROTATE, sender, p, curmsg, ci, cq, cm, sel, dir); break;
                    case N_DELCUBE: skip = spaghetti::simplehook(N_DELCUBE, sender, p, curmsg, ci, cq, cm, sel); break;
                    case N_EDITT:
                    case N_REPLACE:
                    case N_EDITVSLOT:
                    {
                        int size = server::msgsizelookup(type);
                        if(size<=0) { disconnect_client(sender, DISC_MSGERR); return; }
                        switch(type) {
                            case N_EDITT: tex = getint(p); allfaces = getint(p); skip = spaghetti::simplehook(N_EDITT, sender, p, curmsg, ci, cq, cm, sel, tex, allfaces); break;
                            case N_REPLACE: tex = getint(p); newtex = getint(p); insel = getint(p); skip = spaghetti::simplehook(N_REPLACE, sender, p, curmsg, ci, cq, cm, sel, tex, newtex, insel); break;
                            case N_EDITVSLOT: delta = getint(p); allfaces = getint(p); skip = spaghetti::simplehook(N_EDITVSLOT, sender, p, curmsg, ci, cq, cm, sel, delta, allfaces); break;
                        }
                        if(p.remaining() < 2) { disconnect_client(sender, DISC_MSGERR); return; }
                        int extra = lilswap(*(const ushort *)p.pad(2));
                        if(p.remaining() < extra) { disconnect_client(sender, DISC_MSGERR); return; }
                        p.pad(extra);
                        break;
                    }
                }
                if(skip) break;
                if(type == N_COPY)
                {
                    ci->cleanclipboard();
                    ci->lastclipboard = totalmillis ? totalmillis : 1;
                }
                else if(type == N_PASTE) if(ci->state.state!=CS_SPECTATOR) sendclipboard(ci);
                if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
                break;
            }

            case N_REMIP:
                if(spaghetti::simplehook(N_REMIP, sender, p, curmsg, ci, cq, cm)) break;
                if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
                break;

            case N_CLIPBOARD:
            {
                int unpacklen = getint(p), packlen = getint(p); 
                ucharbuf clip = p.subbuf(max(packlen, 0));
                if(spaghetti::simplehook(N_CLIPBOARD, sender, p, curmsg, ci, cq, cm, unpacklen, packlen, clip)) break;
                ci->cleanclipboard(false);
                if(ci->state.state==CS_SPECTATOR) break;
                if(packlen <= 0 || packlen > (1<<16) || unpacklen <= 0) packlen = unpacklen = 0;
                packetbuf q(32 + packlen, ENET_PACKET_FLAG_RELIABLE);
                putint(q, N_CLIPBOARD);
                putint(q, ci->clientnum);
                putint(q, unpacklen);
                putint(q, packlen); 
                q.put(clip.buf, clamp(packlen, 0, clip.maxlen));
                ci->clipboard = q.finalize();
                ci->clipboard->referenceCount++;
                break;
            }

            case N_UNDO:
            case N_REDO:
            {
                int unpacklen = getint(p), packlen = getint(p);
                bool skip = false;
                switch(type) {
                    case N_UNDO: skip = spaghetti::simplehook(N_UNDO, sender, p, curmsg, ci, cq, cm, unpacklen, packlen); break;
                    case N_REDO: skip = spaghetti::simplehook(N_REDO, sender, p, curmsg, ci, cq, cm, unpacklen, packlen); break;
                }
                if(skip) break;
                if(!ci || ci->state.state==CS_SPECTATOR || packlen <= 0 || packlen > (1<<16) || unpacklen <= 0)
                {
                    if(packlen > 0) p.subbuf(packlen);
                    break;
                }
                if(p.remaining() < packlen) { disconnect_client(sender, DISC_MSGERR); return; }
                packetbuf q(32 + packlen, ENET_PACKET_FLAG_RELIABLE);
                putint(q, type);
                putint(q, ci->clientnum);
                putint(q, unpacklen);
                putint(q, packlen);
                if(packlen > 0) p.get(q.subbuf(packlen).buf, packlen);
                sendpacket(-1, 1, q.finalize(), ci->clientnum);
                break;
            }

            case N_SERVCMD:
                getstring(text, p);
                if(!strncmp(text, "__", 2)) break;
                spaghetti::simpleevent(N_SERVCMD, sender, p, curmsg, ci, cq, cm, text);
                break;

            case N_SOUND:
            {
                int sound = getint(p);
                if(spaghetti::simplehook(N_SOUND, sender, p, curmsg, ci, cq, cm, sound)) break;
                if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
                break;
            }

            case N_TAUNT:
                if(spaghetti::simplehook(N_TAUNT, sender, p, curmsg, ci, cq, cm)) break;
                if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
                break;
                     
            #define PARSEMESSAGES 1
            #include "capture.h"
            #include "ctf.h"
            #include "collect.h"
            #undef PARSEMESSAGES

            case -1:
                _type = realtype;
                if(!spaghetti::simplehook(spaghetti::hotstring::martian, sender, p, curmsg, ci, cq, cm, type))
                {
                    disconnect_client(sender, DISC_MSGERR);
                    return;
                }
                else break;

            case -2:
                disconnect_client(sender, DISC_OVERFLOW);
                return;

            default:
            {
                int size = server::msgsizelookup(type);
                if(size<=0) {
                    if(!spaghetti::simplehook(spaghetti::hotstring::martian, sender, p, curmsg, ci, cq, cm, type))
                    {
                        disconnect_client(sender, DISC_MSGERR);
                        return;
                    }
                    break;
                }
                //spaghettimod should never execute this
                loopi(size-1) getint(p);
                if(ci) switch(msgfilter[type])
                {
                    case 2: case 3: if(ci->state.state != CS_SPECTATOR) QUEUE_MSG; break;
                    default: if(cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; } break;
                }
                break;
            }
        }
    }

    int laninfoport() { return SAUERBRATEN_LANINFO_PORT; }
    int serverinfoport(int servport) { return servport < 0 ? SAUERBRATEN_SERVINFO_PORT : servport+1; }
    int serverport(int infoport) { return infoport < 0 ? SAUERBRATEN_SERVER_PORT : infoport-1; }
    const char *defaultmaster() { return "master.sauerbraten.org"; }
    int masterport() { return SAUERBRATEN_MASTER_PORT; }
    int numchannels() { return 3; }

    #include "extinfo.h"

    void serverinforeply(ucharbuf &req, ucharbuf &p)
    {
        if(req.remaining() && !getint(req))
        {
            extserverinforeply(req, p);
            return;
        }

        putint(p, numclients(-1, false, true));
        putint(p, gamepaused || gamespeed != 100 ? 7 : 5);                   // number of attrs following
        putint(p, PROTOCOL_VERSION);    // generic attributes, passed back below
        putint(p, gamemode);
        putint(p, m_timed ? max((gamelimit - gamemillis)/1000, 0) : 0);
        putint(p, maxclients);
        putint(p, serverpass[0] ? MM_PASSWORD : (!m_mp(gamemode) ? MM_PRIVATE : (mastermode || mastermask&MM_AUTOAPPROVE ? mastermode : MM_AUTH)));
        if(gamepaused || gamespeed != 100)
        {
            putint(p, gamepaused ? 1 : 0);
            putint(p, gamespeed);
        }
        sendstring(smapname, p);
        sendstring(serverdesc, p);
        sendserverinforeply(p);
    }

    bool servercompatible(char *name, char *sdec, char *map, int ping, const vector<int> &attr, int np)
    {
        return attr.length() && attr[0]==PROTOCOL_VERSION;
    }

    #include "aiman.h"
}

namespace server{

int captureservmode::CAPTURERADIUS = 64;
int captureservmode::CAPTUREHEIGHT = 24;
int captureservmode::OCCUPYBONUS = 1;
int captureservmode::OCCUPYPOINTS = 1;
int captureservmode::OCCUPYENEMYLIMIT = 28;
int captureservmode::OCCUPYNEUTRALLIMIT = 14;
int captureservmode::SCORESECS = 10;
int captureservmode::AMMOSECS = 15;
int captureservmode::REGENSECS = 1;
int captureservmode::REGENHEALTH = 10;
int captureservmode::REGENARMOUR = 10;
int captureservmode::REGENAMMO = 20;
int captureservmode::MAXAMMO = 5;
int captureservmode::REPAMMODIST = 32;
int captureservmode::RESPAWNSECS = 5;
int captureservmode::MAXBASES = 100;

int ctfservmode::BASERADIUS = 64;
int ctfservmode::BASEHEIGHT = 24;
int ctfservmode::MAXFLAGS = 20;
int ctfservmode::FLAGRADIUS = 16;
int ctfservmode::FLAGLIMIT = 10;
int ctfservmode::MAXHOLDSPAWNS = 100;
int ctfservmode::HOLDSECS = 20;
int ctfservmode::HOLDFLAGS = 1;
int ctfservmode::HOLDDEATHPENALTY = 5;
int ctfservmode::RESPAWNSECS = 5;
int ctfservmode::RESETFLAGTIME = 10000;
int ctfservmode::INVISFLAGTIME = 20000;

int collectservmode::BASERADIUS = 16;
int collectservmode::BASEHEIGHT = 16;
int collectservmode::MAXBASES = 20;
int collectservmode::TOKENRADIUS = 16;
int collectservmode::TOKENLIMIT = 5;
int collectservmode::UNOWNEDTOKENLIMIT = 15;
int collectservmode::TOKENDIST = 16;
int collectservmode::SCORELIMIT = 50;
int collectservmode::RESPAWNSECS = 5;
int collectservmode::EXPIRETOKENTIME = 10000;
int collectservmode::STEALTOKENTIME = 5000;

}

namespace spaghetti{

using namespace luabridge;
using namespace server;

void bindserver(){
    //server
    #define addArray(T)\
         beginClass<T>(#T)\
            .addFunction("__arrayindex", &T::__arrayindex)\
            .addFunction("__arraynewindex", &T::__arraynewindex)\
        .endClass()
    getGlobalNamespace(L).beginNamespace("server")
        .beginClass<gamemodeinfo>("gamemodeinfo")
            .addData("name", &gamemodeinfo::name, false)
            .addData("flags", &gamemodeinfo::flags, false)
            .addData("info", &gamemodeinfo::info, false)
        .endClass()
        .beginClass<itemstat>("itemstat")
            .addData("add", &itemstat::add)
            .addData("max", &itemstat::max)
            .addData("sound", &itemstat::sound)
            .addData("name", &itemstat::name, false)
            .addData("info", &itemstat::info)
        .endClass()
        .beginClass<guninfo>("guninfo")
            .addData("sound", &guninfo::sound)
            .addData("attackdelay", &guninfo::attackdelay)
            .addData("damage", &guninfo::damage)
            .addData("spread", &guninfo::spread)
            .addData("projspeed", &guninfo::projspeed)
            .addData("kickamount", &guninfo::kickamount)
            .addData("range", &guninfo::range)
            .addData("rays", &guninfo::rays)
            .addData("hitpush", &guninfo::hitpush)
            .addData("exprad", &guninfo::exprad)
            .addData("ttl", &guninfo::ttl)
            .addData("name", &guninfo::name, false)
            .addData("file", &guninfo::file, false)
            .addData("part", &guninfo::part)
        .endClass()
        .addArray(fpsstate::lua_array)
        .beginClass<fpsstate>("fpsstate")
            .addData("health", &fpsstate::health)
            .addData("maxhealth", &fpsstate::maxhealth)
            .addData("armour", &fpsstate::armour)
            .addData("armourtype", &fpsstate::armourtype)
            .addData("quadmillis", &fpsstate::quadmillis)
            .addData("gunselect", &fpsstate::gunselect)
            .addData("gunwait", &fpsstate::gunwait)
            .addData("ammo", &fpsstate::ammo)
            .addData("aitype", &fpsstate::aitype)
            .addData("skill", &fpsstate::skill)
            .addFunction("baseammo", &fpsstate::baseammo)
            .addFunction("addammo", &fpsstate::addammo)
            .addFunction("hasmaxammo", &fpsstate::hasmaxammo)
            .addFunction("canpickup", &fpsstate::canpickup)
            .addFunction("pickup", &fpsstate::pickup)
            .addFunction("respawn", &fpsstate::respawn)
            .addFunction("spawnstate", &fpsstate::spawnstate)
            .addFunction("dodamage", &fpsstate::dodamage)
            .addFunction("hasammo", &fpsstate::hasammo)
        .endClass()
        .beginClass<teaminfo>("teaminfo")
            .addData("team", &teaminfo::team)
            .addData("frags", &teaminfo::frags)
        .endClass()
        .beginClass<server_entity>("server_entity")
            .addConstructor<void(*)()>()
            .addData("type", &server_entity::type)
            .addData("spawntime", &server_entity::spawntime)
            .addData("spawned", &server_entity::spawned)
        .endClass()
        .beginClass<servmodeitem>("servmodeitem")
            .addData("tag", &servmodeitem::tag)
            .addData("o", &servmodeitem::o)
        .endClass()
        .addFunction("m_valid", +[](int mode){
            return bool(m_valid(mode));
        })
        .addFunction("m_check", +[](int mode, int flag){
            return bool(m_check(mode, flag));
        })
        .addFunction("m_checknot", +[](int mode, int flag){
            return bool(m_checknot(mode, flag));
        })
        .addFunction("m_checkall", +[](int mode, int flag){
            return bool(m_checkall(mode, flag));
        })
        .addFunction("m_checkonly", +[](int mode, int flag, int exclude){
            return bool(m_checkonly(mode, flag, exclude));
        })
#define bindm(m) .addProperty(#m, +[](){ return bool(m);})
        bindm(m_noitems)
        bindm(m_noammo)
        bindm(m_insta)
        bindm(m_tactics)
        bindm(m_efficiency)
        bindm(m_capture)
        bindm(m_capture_only)
        bindm(m_regencapture)
        bindm(m_ctf)
        bindm(m_ctf_only)
        bindm(m_protect)
        bindm(m_hold)
        bindm(m_collect)
        bindm(m_teammode)
        .addFunction("isteam", +[](const char* a, const char* b){
            return bool(isteam(a, b));
        })
        bindm(m_demo)
        bindm(m_edit)
        bindm(m_lobby)
        bindm(m_timed)
        bindm(m_botmode)
        .addFunction("m_mp", +[](int mode){
            return bool(m_mp(mode));
        })
        bindm(m_sp)
        bindm(m_dmsp)
        bindm(m_classicsp)
#undef bindm
        .addFunction("isaitype", +[](int t){
            return bool(isaitype(t));
        })
        .beginNamespace("aiman")
            .addFunction("addai", &aiman::addai)
            .addFunction("reqadd", &aiman::reqadd)
            .addFunction("setbotlimit", &aiman::setbotlimit)
            .addFunction("setbotbalance", &aiman::setbotbalance)
            .addFunction("changeteam", &aiman::changeteam)
            .addFunction("deleteai", (bool(*)())&aiman::deleteai)
            .addFunction("reqdel", &aiman::reqdel)
        .endNamespace()
        .beginClass<gameevent>("gameevent")
            .addFunction("flush", &gameevent::flush)
            .addFunction("process", &gameevent::process)
            .addFunction("keepable", &gameevent::keepable)
        .endClass()
        .deriveClass<timedevent, gameevent>("timedevent")
            .addData("millis", &timedevent::millis)
        .endClass()
        .beginClass<hitinfo>("hitinfo")
            .addConstructor<void(*)()>()
            .addData("target", &hitinfo::target)
            .addData("lifesequence", &hitinfo::lifesequence)
            .addData("rays", &hitinfo::rays)
            .addData("dist", &hitinfo::dist)
            .addData("dir", &hitinfo::dir)
        .endClass()
        .deriveClass<shotevent, timedevent>("shotevent")
            .addConstructor<void(*)()>()
            .addData("id", &shotevent::id)
            .addData("gun", &shotevent::gun)
            .addData("from", &shotevent::from)
            .addData("to", &shotevent::to)
            .addData("hits", &shotevent::hits)
        .endClass()
        .deriveClass<explodeevent, timedevent>("explodeevent")
            .addConstructor<void(*)()>()
            .addData("id", &explodeevent::id)
            .addData("gun", &explodeevent::gun)
            .addData("hits", &explodeevent::hits)
        .endClass()
        .deriveClass<suicideevent, gameevent>("suicideevent")
            .addConstructor<void(*)()>()
        .endClass()
        .deriveClass<pickupevent, gameevent>("pickupevent")
            .addConstructor<void(*)()>()
            .addData("ent", &pickupevent::ent)
        .endClass()
        .addArray(projectilestate<8>::lua_array)
        .beginClass<projectilestate<8>>("projectilestate<8>")
            .addData("projs", &projectilestate<8>::projs)
            .addData("numprojs", &projectilestate<8>::numprojs)
            .addFunction("reset", &projectilestate<8>::reset)
            .addFunction("add", &projectilestate<8>::add)
            .addFunction("remove", &projectilestate<8>::remove)
        .endClass()
        .deriveClass<gamestate, fpsstate>("gamestate")
            .addData("o", &gamestate::o)
            .addData("state", &gamestate::state)
            .addData("editstate", &gamestate::editstate)
            .addData("lastdeath", &gamestate::lastdeath)
            .addData("deadflush", &gamestate::deadflush)
            .addData("lastspawn", &gamestate::lastspawn)
            .addData("lifesequence", &gamestate::lifesequence)
            .addData("lastshot", &gamestate::lastshot)
            .addData("rockets", &gamestate::rockets)
            .addData("grenades", &gamestate::grenades)
            .addData("frags", &gamestate::frags)
            .addData("flags", &gamestate::flags)
            .addData("deaths", &gamestate::deaths)
            .addData("teamkills", &gamestate::teamkills)
            .addData("shotdamage", &gamestate::shotdamage)
            .addData("damage", &gamestate::damage)
            .addData("tokens", &gamestate::tokens)
            .addData("lasttimeplayed", &gamestate::lasttimeplayed)
            .addData("timeplayed", &gamestate::timeplayed)
            .addData("effectiveness", &gamestate::effectiveness)
            .addFunction("isalive", &gamestate::isalive)
            .addFunction("waitexpired", &gamestate::waitexpired)
            .addFunction("reset", &gamestate::reset)
            .addFunction("respawn", &gamestate::respawn)
            .addFunction("reassign", &gamestate::reassign)
        .endClass()
        .beginClass<savedscore>("savedscore")
            .addData("ip", &savedscore::ip)
            .addData("name", &savedscore::name)
            .addData("frags", &savedscore::frags)
            .addData("flags", &savedscore::flags)
            .addData("deaths", &savedscore::deaths)
            .addData("teamkills", &savedscore::teamkills)
            .addData("shotdamage", &savedscore::shotdamage)
            .addData("damage", &savedscore::damage)
            .addData("extra", &savedscore::extra)
        .endClass()
        .beginClass<clientinfo>("clientinfo")
            .addData("clientnum", &clientinfo::clientnum)
            .addData("ownernum", &clientinfo::ownernum)
            .addData("connectmillis", &clientinfo::connectmillis)
            .addData("sessionid", &clientinfo::sessionid)
            .addData("overflow", &clientinfo::overflow)
            .addData("name", &clientinfo::name)
            .addData("team", &clientinfo::team)
            .addData("mapvote", &clientinfo::mapvote)
            .addData("playermodel", &clientinfo::playermodel)
            .addData("modevote", &clientinfo::modevote)
            .addData("privilege", &clientinfo::privilege)
            .addData("connected", &clientinfo::connected)
            .addData("local", &clientinfo::local)
            .addData("timesync", &clientinfo::timesync)
            .addData("gameoffset", &clientinfo::gameoffset)
            .addData("lastevent", &clientinfo::lastevent)
            .addData("pushed", &clientinfo::pushed)
            .addData("exceeded", &clientinfo::exceeded)
            .addData("state", &clientinfo::state)
            .addData("events", &clientinfo::events)
            .addData("position", &clientinfo::position)
            .addData("messages", &clientinfo::messages)
            .addData("bots", &clientinfo::bots)
            .addData("ping", &clientinfo::ping)
            .addData("aireinit", &clientinfo::aireinit)
            .addData("clientmap", &clientinfo::clientmap)
            .addData("mapcrc", &clientinfo::mapcrc)
            .addData("warned", &clientinfo::warned)
            .addData("gameclip", &clientinfo::gameclip)
            .addData("getdemo", &clientinfo::getdemo)
            .addData("getmap", &clientinfo::getmap)
            .addData("clipboard", &clientinfo::clipboard)
            .addData("lastclipboard", &clientinfo::lastclipboard)
            .addData("needclipboard", &clientinfo::needclipboard)
            .addData("connectauth", &clientinfo::connectauth)
            .addData("authreq", &clientinfo::authreq)
            .addData("authname", &clientinfo::authname)
            .addData("authdesc", &clientinfo::authdesc)
            .addData("authkickvictim", &clientinfo::authkickvictim)
            .addData("authkickreason", &clientinfo::authkickreason)
            .addData("extra", &clientinfo::extra)
            .addFunction("addevent", &clientinfo::addevent)
            .addFunction("calcpushrange", &clientinfo::calcpushrange)
            .addFunction("checkpushed", &clientinfo::checkpushed)
            .addFunction("scheduleexceeded", &clientinfo::scheduleexceeded)
            .addFunction("setexceeded", &clientinfo::setexceeded)
            .addFunction("setpushed", &clientinfo::setpushed)
            .addFunction("checkexceeded", &clientinfo::checkexceeded)
            .addFunction("mapchange", &clientinfo::mapchange)
            .addFunction("reassign", &clientinfo::reassign)
            .addFunction("cleanclipboard", &clientinfo::cleanclipboard)
            .addFunction("cleanauthkick", &clientinfo::cleanauthkick)
            .addFunction("cleanauth", &clientinfo::cleanauth)
            .addFunction("reset", &clientinfo::reset)
            .addFunction("geteventmillis", &clientinfo::geteventmillis)
        .endClass()
        .beginClass<servmode>("servmode")
            .addFunction("entergame", &servmode::entergame)
            .addFunction("leavegame", &servmode::leavegame)
            .addFunction("moved", &servmode::moved)
            .addFunction("canspawn", &servmode::canspawn)
            .addFunction("spawned", &servmode::spawned)
            .addFunction("fragvalue", &servmode::fragvalue)
            .addFunction("died", &servmode::died)
            .addFunction("canchangeteam", &servmode::canchangeteam)
            .addFunction("changeteam", &servmode::changeteam)
            .addFunction("initclient", &servmode::initclient)
            .addFunction("update", &servmode::update)
            .addFunction("cleanup", &servmode::cleanup)
            .addFunction("setup", &servmode::setup)
            .addFunction("newmap", &servmode::newmap)
            .addFunction("intermission", &servmode::intermission)
            .addFunction("hidefrags", &servmode::hidefrags)
            .addFunction("getteamscore", &servmode::getteamscore)
            .addFunction("extinfoteam", &servmode::extinfoteam)
            .addFunction("parseitems", &servmode::parseitems)
        .endClass()
        .beginClass<captureservmode::baseinfo>("captureservmode::baseinfo")
            .addConstructor<void(*)()>()
            .addData("o", &captureservmode::baseinfo::o)
            .addData("owner", &captureservmode::baseinfo::owner)
            .addData("enemy", &captureservmode::baseinfo::enemy)
            .addData("ammogroup", &captureservmode::baseinfo::ammogroup)
            .addData("ammotype", &captureservmode::baseinfo::ammotype)
            .addData("ammo", &captureservmode::baseinfo::ammo)
            .addData("owners", &captureservmode::baseinfo::owners)
            .addData("enemies", &captureservmode::baseinfo::enemies)
            .addData("converted", &captureservmode::baseinfo::converted)
            .addData("capturetime", &captureservmode::baseinfo::capturetime)
            .addFunction("valid", &captureservmode::baseinfo::valid)
            .addFunction("noenemy", &captureservmode::baseinfo::noenemy)
            .addFunction("reset", &captureservmode::baseinfo::reset)
            .addFunction("enter", &captureservmode::baseinfo::enter)
            .addFunction("steal", &captureservmode::baseinfo::steal)
            .addFunction("leave", &captureservmode::baseinfo::leave)
            .addFunction("occupy", &captureservmode::baseinfo::occupy)
            .addFunction("addammo", &captureservmode::baseinfo::addammo)
            .addFunction("takeammo", &captureservmode::baseinfo::takeammo)
        .endClass()
        .beginClass<captureservmode::score>("captureservmode::score")
            .addConstructor<void(*)()>()
            .addData("team", &captureservmode::score::team)
            .addData("total", &captureservmode::score::total)
        .endClass()
        .deriveClass<captureservmode, servmode>("captureservmode")
            .addStaticData("CAPTURERADIUS", &captureservmode::CAPTURERADIUS)
            .addStaticData("CAPTUREHEIGHT", &captureservmode::CAPTUREHEIGHT)
            .addStaticData("OCCUPYBONUS", &captureservmode::OCCUPYBONUS)
            .addStaticData("OCCUPYPOINTS", &captureservmode::OCCUPYPOINTS)
            .addStaticData("OCCUPYENEMYLIMIT", &captureservmode::OCCUPYENEMYLIMIT)
            .addStaticData("OCCUPYNEUTRALLIMIT", &captureservmode::OCCUPYNEUTRALLIMIT)
            .addStaticData("SCORESECS", &captureservmode::SCORESECS)
            .addStaticData("AMMOSECS", &captureservmode::AMMOSECS)
            .addStaticData("REGENSECS", &captureservmode::REGENSECS)
            .addStaticData("REGENHEALTH", &captureservmode::REGENHEALTH)
            .addStaticData("REGENARMOUR", &captureservmode::REGENARMOUR)
            .addStaticData("REGENAMMO", &captureservmode::REGENAMMO)
            .addStaticData("MAXAMMO", &captureservmode::MAXAMMO)
            .addStaticData("REPAMMODIST", &captureservmode::REPAMMODIST)
            .addStaticData("RESPAWNSECS", &captureservmode::RESPAWNSECS)
            .addStaticData("MAXBASES", &captureservmode::MAXBASES)
            .addData("bases", &captureservmode::bases)
            .addData("scores", &captureservmode::scores)
            .addData("captures", &captureservmode::captures)
            .addFunction("resetbases", &captureservmode::resetbases)
            .addFunction("findscore", &captureservmode::findscore)
            .addFunction("addbase", &captureservmode::addbase)
            .addFunction("initbase", &captureservmode::initbase)
            .addFunction("hasbases", &captureservmode::hasbases)
            .addFunction("insidebase", &captureservmode::insidebase)
            .addData("notgotbases", &captureservmode::notgotbases)
            .addFunction("reset", &captureservmode::reset)
            .addFunction("stealbase", &captureservmode::stealbase)
            .addFunction("replenishammo", &captureservmode::replenishammo)
            .addFunction("movebases", &captureservmode::movebases)
            .addFunction("leavebases", &captureservmode::leavebases)
            .addFunction("enterbases", &captureservmode::enterbases)
            .addFunction("addscore", &captureservmode::addscore)
            .addFunction("regenowners", &captureservmode::regenowners)
            .addFunction("sendbaseinfo", &captureservmode::sendbaseinfo)
            .addFunction("sendbases", &captureservmode::sendbases)
            .addFunction("endcheck", &captureservmode::endcheck)
            .addFunction("parsebases", &captureservmode::parsebases)
            .addFunction("extinfoteam", &captureservmode::extinfoteam)
        .endClass()
        .addFunction("ctfteamflag", +[](const char* s){
            return int(ctfteamflag(s));
        })
        .addFunction("ctfflagteam", +[](int i){
            return std::string(ctfflagteam(i));
        })
        .beginClass<ctfservmode::flag>("ctfservmode::flag")
            .addConstructor<void(*)()>()
            .addData("id", &ctfservmode::flag::id)
            .addData("version", &ctfservmode::flag::version)
            .addData("spawnindex", &ctfservmode::flag::spawnindex)
            .addData("droploc", &ctfservmode::flag::droploc)
            .addData("spawnloc", &ctfservmode::flag::spawnloc)
            .addData("owner", &ctfservmode::flag::owner)
            .addData("dropcount", &ctfservmode::flag::dropcount)
            .addData("dropper", &ctfservmode::flag::dropper)
            .addData("invistime", &ctfservmode::flag::invistime)
            .addFunction("reset", &ctfservmode::flag::reset)
        .endClass()
        .beginClass<ctfservmode::holdspawn>("ctfservmode::holdspawn")
            .addConstructor<void(*)()>()
            .addData("o", &ctfservmode::holdspawn::o)
        .endClass()
        .deriveClass<ctfservmode, servmode>("ctfservmode")
            .addStaticData("BASERADIUS", &ctfservmode::BASERADIUS)
            .addStaticData("BASEHEIGHT", &ctfservmode::BASEHEIGHT)
            .addStaticData("MAXFLAGS", &ctfservmode::MAXFLAGS)
            .addStaticData("FLAGRADIUS", &ctfservmode::FLAGRADIUS)
            .addStaticData("FLAGLIMIT", &ctfservmode::FLAGLIMIT)
            .addStaticData("MAXHOLDSPAWNS", &ctfservmode::MAXHOLDSPAWNS)
            .addStaticData("HOLDSECS", &ctfservmode::HOLDSECS)
            .addStaticData("HOLDFLAGS", &ctfservmode::HOLDFLAGS)
            .addStaticData("RESPAWNSECS", &ctfservmode::RESPAWNSECS)
            .addStaticData("RESETFLAGTIME", &ctfservmode::RESETFLAGTIME)
            .addStaticData("INVISFLAGTIME", &ctfservmode::INVISFLAGTIME)
            .addData("holdspawns", &ctfservmode::holdspawns)
            .addData("flags", &ctfservmode::flags)
            .addData("notgotflags", &ctfservmode::notgotflags)
            .addFunction("resetflags", &ctfservmode::resetflags)
            .addFunction("addflag", &ctfservmode::addflag)
            .addFunction("addholdspawn", &ctfservmode::addholdspawn)
            .addFunction("ownflag", &ctfservmode::ownflag)
            .addFunction("dropflag", (void(ctfservmode::*)(int, const vec&, int, int, bool))&ctfservmode::dropflag)
            .addFunction("dropflagci", (void(ctfservmode::*)(clientinfo*, clientinfo*))&ctfservmode::dropflag)
            .addFunction("returnflag", &ctfservmode::returnflag)
            .addFunction("totalscore", &ctfservmode::totalscore)
            .addFunction("setscore", &ctfservmode::setscore)
            .addFunction("addscore", &ctfservmode::addscore)
            .addFunction("insidebase", &ctfservmode::insidebase)
            .addFunction("reset", &ctfservmode::reset)
            .addFunction("setupholdspawns", &ctfservmode::setupholdspawns)
            .addFunction("spawnflag", &ctfservmode::spawnflag)
            .addFunction("scoreflag", &ctfservmode::scoreflag)
            .addFunction("takeflag", &ctfservmode::takeflag)
            .addFunction("parseflags", &ctfservmode::parseflags)
        .endClass()
        .addFunction("collectteambase", +[](const char* s){
            return int(collectteambase(s));
        })
        .addFunction("collectbaseteam", +[](int i){
            return std::string(collectbaseteam(i));
        })
        .beginClass<collectservmode::base>("collectservmode::base")
            .addConstructor<void(*)()>()
            .addData("id", &collectservmode::base::id)
            .addData("team", &collectservmode::base::team)
            .addData("o", &collectservmode::base::o)
            .addData("laststeal", &collectservmode::base::laststeal)
            .addFunction("reset", &collectservmode::base::reset)
        .endClass()
        .beginClass<collectservmode::token>("collectservmode::token")
            .addConstructor<void(*)()>()
            .addData("id", &collectservmode::token::id)
            .addData("team", &collectservmode::token::team)
            .addData("droptime", &collectservmode::token::droptime)
            .addData("o", &collectservmode::token::o)
            .addData("yaw", &collectservmode::token::yaw)
            .addData("dropper", &collectservmode::token::dropper)
            .addFunction("reset", &collectservmode::reset)
        .endClass()
        .deriveClass<collectservmode, servmode>("collectservmode")
            .addData("bases", &collectservmode::bases)
            .addData("tokens", &collectservmode::tokens)
            .addData("nexttoken", &collectservmode::nexttoken)
            .addData("notgotbases", &collectservmode::notgotbases)
            .addFunction("resetbases", &collectservmode::resetbases)
            .addFunction("addbase", &collectservmode::addbase)
            .addFunction("findtoken", &collectservmode::findtoken)
            .addFunction("droptoken", &collectservmode::droptoken)
            .addFunction("removetoken", &collectservmode::removetoken)
            .addFunction("totalscore", &collectservmode::totalscore)
            .addFunction("setscore", &collectservmode::setscore)
            .addFunction("addscore", &collectservmode::addscore)
            .addFunction("insidebase", &collectservmode::insidebase)
            .addFunction("droptokens", &collectservmode::droptokens)
            .addFunction("deposittokens", &collectservmode::deposittokens)
            .addFunction("taketoken", &collectservmode::taketoken)
            .addFunction("parsebases", &collectservmode::parsebases)
        .endClass()
    .endNamespace();
    bindArrayProxy<decltype(gamemodes)>("server");
    bindArrayProxy<decltype(itemstats)>("server");
    bindArrayProxy<decltype(guns)>("server");
    bindVectorOf<server_entity>("server");
    bindVectorOf<servmodeitem>("server");
    bindVectorOf<hitinfo>("server");
    bindVectorOf<clientinfo*>("server");
    bindVectorOf<gameevent*>("server");
    bindVectorOf<captureservmode::baseinfo>("server");
    bindVectorOf<captureservmode::score>("server");
    bindVectorOf<ctfservmode::holdspawn>("server");
    bindVectorOf<ctfservmode::flag>("server");
    bindVectorOf<collectservmode::base>("server");
    bindVectorOf<collectservmode::token>("server");
    bindVectorOf<savedscore>("server");
    getGlobalNamespace(L).beginNamespace("server")
        .addVariable("MAXBOTS", &MAXBOTS)
        .addVariable("DEATHMILLIS", &DEATHMILLIS)
        .addVariable("notgotitems", &notgotitems)
        .addVariable("gamemode", &gamemode)
        .addVariable("gamemillis", &gamemillis)
        .addVariable("gamelimit", &gamelimit)
        .addVariable("gamespeed", &server::gamespeed)
        .addVariable("gamepaused", &gamepaused)
        .addVariable("shouldstep", &shouldstep)
        .addProperty("smapname", +[]{ return (const char*)smapname; }, +[](const char* s){ copystring(smapname, s); })
        .addVariable("interm", &interm)
        .addVariable("lastsend", &lastsend)
        .addVariable("mastermode", &mastermode)
        .addVariable("mastermask", &mastermask)
        .addVariable("demonextmatch", &demonextmatch)
        .addVariable("mcrc", &mcrc)
        .addVariable("smode", &smode)
        .addVariable("reliablemessages", &reliablemessages)
        .addCFunction("loadents", +[](lua_State* L){
            uint crc;
            const char* map = Stack<const char*>::get(L, 1);
            auto& vec = Stack<vector<entity>&>::get(L, 2);
            if(loadents(map, vec, &crc)){
                lua_pushinteger(L, crc);
                return 1;
            }
            return 0;
        })
        .addFunction("kickclients", kickclients)
        .addFunction("getinfo", getinfo)
        .addFunction("modename", modename)
        .addFunction("mastermodename", mastermodename)
        .addFunction("hashpassword", +[](int cn, int sessionid, const char *pwd){
            string buff;
            hashpassword(cn, sessionid, pwd, buff);
            return std::string(buff);
        })
        .addFunction("msgsizelookup", msgsizelookup)
        .addFunction("privname", privname)
        .addFunction("sendservmsg", sendservmsg)
        .addFunction("resetitems", resetitems)
        .addFunction("serverinit", serverinit)
        .addFunction("numclients", numclients)
        .addFunction("duplicatename", duplicatename)
        .addFunction("colorname", colorname)
        .addFunction("canspawnitem", canspawnitem)
        .addFunction("spawntime", spawntime)
        .addFunction("delayspawn", delayspawn)
        .addFunction("pickup", pickup)
        .addFunction("clearteaminfo", clearteaminfo)
        .addFunction("teamhasplayers", teamhasplayers)
        .addFunction("pruneteaminfo", pruneteaminfo)
        .addFunction("addteaminfo", addteaminfo)
        .addFunction("choosebestclient", choosebestclient)
        .addFunction("autoteam", autoteam)
        .addFunction("chooseworstteam", chooseworstteam)
        .addFunction("prunedemos", prunedemos)
        .addFunction("adddemo", adddemo)
        .addFunction("enddemorecord", enddemorecord)
        .addFunction("recordpacket", +[](int chan, std::string data){
            recordpacket(chan, (void*)(data.data()), data.size());
        })
        .addFunction("setupdemorecord", setupdemorecord)
        .addFunction("listdemos", listdemos)
        .addFunction("cleardemos", cleardemos)
        .addFunction("freegetmap", freegetmap)
        .addFunction("freegetdemo", freegetdemo)
        .addFunction("senddemo", senddemo)
        .addFunction("stopdemo", stopdemo)
        .addFunction("pausegame", pausegame)
        .addFunction("checkpausegame", checkpausegame)
        .addFunction("forcepaused", forcepaused)
        .addFunction("ispaused", ispaused)
        .addFunction("changegamespeed", changegamespeed)
        .addFunction("forcegamespeed", forcegamespeed)
        .addFunction("scaletime", scaletime)
        .addFunction("checkpassword", checkpassword)
        .addFunction("revokemaster", revokemaster)
        .addFunction("connected", connected)
        .addFunction("setmaster", setmaster)
        .addFunction("trykick", trykick)
        .addFunction("findscore", findscore)
        .addFunction("savescore", savescore)
        .addFunction("flushclientposition", flushclientposition)
        .addFunction("buildworldstate", buildworldstate)
        .addFunction("sendpackets", sendpackets)
        .addFunction("sendstate", (void(*)(gamestate&, packetbuf&))sendstate)
        .addFunction("sendstateu", (void(*)(gamestate&, ucharbuf&))sendstate)
        .addFunction("sendstatev", (void(*)(gamestate&, vector<uchar>&))sendstate)
        .addFunction("spawnstate", spawnstate)
        .addFunction("sendspawn", sendspawn)
        .addFunction("sendwelcome", sendwelcome)
        .addFunction("putinitclient", putinitclient)
        .addFunction("welcomeinitclient", welcomeinitclient)
        .addFunction("hasmap", hasmap)
        .addFunction("welcomepacket", welcomepacket)
        .addFunction("restorescore", restorescore)
        .addFunction("sendresume", sendresume)
        .addFunction("sendinitclient", sendinitclient)
        .addFunction("loaditems", loaditems)
        .addFunction("changemap", changemap)
        .addFunction("rotatemap", rotatemap)
        .addFunction("checkvotes", checkvotes)
        .addFunction("forcemap", forcemap)
        .addFunction("vote", vote)
        .addFunction("checkovertime", checkovertime)
        .addFunction("checkintermission", checkintermission)
        .addFunction("startintermission", startintermission)
        .addFunction("dodamage", dodamage)
        .addFunction("suicide", suicide)
        .addFunction("clearevent", clearevent)
        .addFunction("flushevents", flushevents)
        .addFunction("processevents", processevents)
        .addFunction("cleartimedevents", cleartimedevents)
        .addFunction("serverupdate", serverupdate)
        .addFunction("forcespectator", forcespectator)
        .addFunction("checkmaps", checkmaps)
        .addFunction("unspectate", unspectate)
        .addFunction("sendservinfo", sendservinfo)
        .addFunction("noclients", noclients)
        .addFunction("localconnect", server::localconnect)
        .addFunction("localdisconnect", localdisconnect)
        .addFunction("clientconnect", clientconnect)
        .addFunction("clientdisconnect", clientdisconnect)
        .addFunction("reserveclients", reserveclients)
        .beginClass<banlist>("banlist")
            .addFunction("clear", &banlist::clear)
            .addFunction("check", &banlist::check)
            .addFunction("add", &banlist::add)
        .endClass()
        .addVariable("ipbans", &ipbans)
        .addVariable("gbans", &gbans)
        .addFunction("checkbans", checkbans)
        .addFunction("verifybans", verifybans)
        .addFunction("allowconnect", allowconnect)
        .addFunction("allowbroadcast", allowbroadcast)
        .addFunction("findauth", findauth)
        .addFunction("authfailed", (void(*)(uint))authfailed)
        .addFunction("authsucceeded", authsucceeded)
        .addFunction("authchallenged", authchallenged)
        .addFunction("tryauth", tryauth)
        .addFunction("answerchallenge", +[](clientinfo *ci, uint id, const char *_val, const char *desc){
            string val;
            copystring(val, _val);
            return answerchallenge(ci, id, val, desc);
        })
        .addFunction("masterdisconnected", masterdisconnected)
        .addFunction("processmasterinput", +[](const char* cmd){
            processmasterinput(cmd, strlen(cmd), 0);
        })
        .addFunction("receivefile", +[](int sender, std::string data){
            receivefile(sender, (uchar*)data.data(), data.size());
        })
        .addFunction("sendclipboard", sendclipboard)
        .addFunction("parsepacket", parsepacket)
        .addFunction("extinfoplayer", extinfoplayer)
        .addFunction("extinfoteamscore", extinfoteamscore)
        .addFunction("extinfoteams", extinfoteams)
        .addFunction("extserverinforeply", extserverinforeply)
        .addFunction("serverinforeply", serverinforeply)
    .endNamespace();
    //cfr engine/server.cpp
#define addEnum(n)    lua_pushstring(L, #n); lua_pushnumber(L, n); lua_rawset(L, -3)
    lua_getglobal(L, "server");
    addEnum(DMF);
    addEnum(DNF);
    addEnum(DVELF);
    addEnum(NOTUSED);
    addEnum(LIGHT);
    addEnum(MAPMODEL);
    addEnum(PLAYERSTART);
    addEnum(ENVMAP);
    addEnum(PARTICLES);
    addEnum(MAPSOUND);
    addEnum(I_SHELLS);
    addEnum(I_BULLETS);
    addEnum(I_ROCKETS);
    addEnum(I_ROUNDS);
    addEnum(I_GRENADES);
    addEnum(I_CARTRIDGES);
    addEnum(I_HEALTH);
    addEnum(I_BOOST);
    addEnum(I_GREENARMOUR);
    addEnum(I_YELLOWARMOUR);
    addEnum(I_QUAD);
    addEnum(TELEPORT);
    addEnum(TELEDEST);
    addEnum(MONSTER);
    addEnum(CARROT);
    addEnum(JUMPPAD);
    addEnum(BASE);
    addEnum(RESPAWNPOINT);
    addEnum(BOX);
    addEnum(BARREL);
    addEnum(PLATFORM);
    addEnum(ELEVATOR);
    addEnum(FLAG);
    addEnum(MAXENTTYPES);
    addEnum(GUN_FIST);
    addEnum(GUN_SG);
    addEnum(GUN_CG);
    addEnum(GUN_RL);
    addEnum(GUN_RIFLE);
    addEnum(GUN_GL);
    addEnum(GUN_PISTOL);
    addEnum(GUN_FIREBALL);
    addEnum(GUN_ICEBALL);
    addEnum(GUN_SLIMEBALL);
    addEnum(GUN_BITE);
    addEnum(GUN_BARREL);
    addEnum(NUMGUNS);
    addEnum(A_BLUE);
    addEnum(A_GREEN);
    addEnum(A_YELLOW);
    addEnum(M_TEAM);
    addEnum(M_NOITEMS);
    addEnum(M_NOAMMO);
    addEnum(M_INSTA);
    addEnum(M_EFFICIENCY);
    addEnum(M_TACTICS);
    addEnum(M_CAPTURE);
    addEnum(M_REGEN);
    addEnum(M_CTF);
    addEnum(M_PROTECT);
    addEnum(M_HOLD);
    addEnum(M_EDIT);
    addEnum(M_DEMO);
    addEnum(M_LOCAL);
    addEnum(M_LOBBY);
    addEnum(M_DMSP);
    addEnum(M_CLASSICSP);
    addEnum(M_SLOWMO);
    addEnum(M_COLLECT);
    lua_pushstring(L, "gamemodes"); push(L, lua_arrayproxy<decltype(gamemodes)>(gamemodes)); lua_rawset(L, -3);
    addEnum(STARTGAMEMODE);
    addEnum(NUMGAMEMODES);
    addEnum(MM_AUTH);
    addEnum(MM_OPEN);
    addEnum(MM_VETO);
    addEnum(MM_LOCKED);
    addEnum(MM_PRIVATE);
    addEnum(MM_PASSWORD);
    addEnum(MM_START);
    addEnum(MM_MODE);
    addEnum(MM_AUTOAPPROVE);
    addEnum(MM_PRIVSERV);
    addEnum(MM_PUBSERV);
    addEnum(MM_COOPSERV);
    addEnum(S_JUMP);
    addEnum(S_LAND);
    addEnum(S_RIFLE);
    addEnum(S_PUNCH1);
    addEnum(S_SG);
    addEnum(S_CG);
    addEnum(S_RLFIRE);
    addEnum(S_RLHIT);
    addEnum(S_WEAPLOAD);
    addEnum(S_ITEMAMMO);
    addEnum(S_ITEMHEALTH);
    addEnum(S_ITEMARMOUR);
    addEnum(S_ITEMPUP);
    addEnum(S_ITEMSPAWN);
    addEnum(S_TELEPORT);
    addEnum(S_NOAMMO);
    addEnum(S_PUPOUT);
    addEnum(S_PAIN1);
    addEnum(S_PAIN2);
    addEnum(S_PAIN3);
    addEnum(S_PAIN4);
    addEnum(S_PAIN5);
    addEnum(S_PAIN6);
    addEnum(S_DIE1);
    addEnum(S_DIE2);
    addEnum(S_FLAUNCH);
    addEnum(S_FEXPLODE);
    addEnum(S_SPLASH1);
    addEnum(S_SPLASH2);
    addEnum(S_GRUNT1);
    addEnum(S_GRUNT2);
    addEnum(S_RUMBLE);
    addEnum(S_PAINO);
    addEnum(S_PAINR);
    addEnum(S_DEATHR);
    addEnum(S_PAINE);
    addEnum(S_DEATHE);
    addEnum(S_PAINS);
    addEnum(S_DEATHS);
    addEnum(S_PAINB);
    addEnum(S_DEATHB);
    addEnum(S_PAINP);
    addEnum(S_PIGGR2);
    addEnum(S_PAINH);
    addEnum(S_DEATHH);
    addEnum(S_PAIND);
    addEnum(S_DEATHD);
    addEnum(S_PIGR1);
    addEnum(S_ICEBALL);
    addEnum(S_SLIMEBALL);
    addEnum(S_JUMPPAD);
    addEnum(S_PISTOL);
    addEnum(S_V_BASECAP);
    addEnum(S_V_BASELOST);
    addEnum(S_V_FIGHT);
    addEnum(S_V_BOOST);
    addEnum(S_V_BOOST10);
    addEnum(S_V_QUAD);
    addEnum(S_V_QUAD10);
    addEnum(S_V_RESPAWNPOINT);
    addEnum(S_FLAGPICKUP);
    addEnum(S_FLAGDROP);
    addEnum(S_FLAGRETURN);
    addEnum(S_FLAGSCORE);
    addEnum(S_FLAGRESET);
    addEnum(S_BURN);
    addEnum(S_CHAINSAW_ATTACK);
    addEnum(S_CHAINSAW_IDLE);
    addEnum(S_HIT);
    addEnum(S_FLAGFAIL);
    addEnum(PRIV_NONE);
    addEnum(PRIV_MASTER);
    addEnum(PRIV_AUTH);
    addEnum(PRIV_ADMIN);
    addEnum(N_CONNECT);
    addEnum(N_SERVINFO);
    addEnum(N_WELCOME);
    addEnum(N_INITCLIENT);
    addEnum(N_POS);
    addEnum(N_TEXT);
    addEnum(N_SOUND);
    addEnum(N_CDIS);
    addEnum(N_SHOOT);
    addEnum(N_EXPLODE);
    addEnum(N_SUICIDE);
    addEnum(N_DIED);
    addEnum(N_DAMAGE);
    addEnum(N_HITPUSH);
    addEnum(N_SHOTFX);
    addEnum(N_EXPLODEFX);
    addEnum(N_TRYSPAWN);
    addEnum(N_SPAWNSTATE);
    addEnum(N_SPAWN);
    addEnum(N_FORCEDEATH);
    addEnum(N_GUNSELECT);
    addEnum(N_TAUNT);
    addEnum(N_MAPCHANGE);
    addEnum(N_MAPVOTE);
    addEnum(N_TEAMINFO);
    addEnum(N_ITEMSPAWN);
    addEnum(N_ITEMPICKUP);
    addEnum(N_ITEMACC);
    addEnum(N_TELEPORT);
    addEnum(N_JUMPPAD);
    addEnum(N_PING);
    addEnum(N_PONG);
    addEnum(N_CLIENTPING);
    addEnum(N_TIMEUP);
    addEnum(N_FORCEINTERMISSION);
    addEnum(N_SERVMSG);
    addEnum(N_ITEMLIST);
    addEnum(N_RESUME);
    addEnum(N_EDITMODE);
    addEnum(N_EDITENT);
    addEnum(N_EDITF);
    addEnum(N_EDITT);
    addEnum(N_EDITM);
    addEnum(N_FLIP);
    addEnum(N_COPY);
    addEnum(N_PASTE);
    addEnum(N_ROTATE);
    addEnum(N_REPLACE);
    addEnum(N_DELCUBE);
    addEnum(N_REMIP);
    addEnum(N_NEWMAP);
    addEnum(N_GETMAP);
    addEnum(N_SENDMAP);
    addEnum(N_CLIPBOARD);
    addEnum(N_EDITVAR);
    addEnum(N_EDITVSLOT);
    addEnum(N_UNDO);
    addEnum(N_REDO);
    addEnum(N_MASTERMODE);
    addEnum(N_KICK);
    addEnum(N_CLEARBANS);
    addEnum(N_CURRENTMASTER);
    addEnum(N_SPECTATOR);
    addEnum(N_SETMASTER);
    addEnum(N_SETTEAM);
    addEnum(N_BASES);
    addEnum(N_BASEINFO);
    addEnum(N_BASESCORE);
    addEnum(N_REPAMMO);
    addEnum(N_BASEREGEN);
    addEnum(N_ANNOUNCE);
    addEnum(N_LISTDEMOS);
    addEnum(N_SENDDEMOLIST);
    addEnum(N_GETDEMO);
    addEnum(N_SENDDEMO);
    addEnum(N_DEMOPLAYBACK);
    addEnum(N_RECORDDEMO);
    addEnum(N_STOPDEMO);
    addEnum(N_CLEARDEMOS);
    addEnum(N_TAKEFLAG);
    addEnum(N_RETURNFLAG);
    addEnum(N_RESETFLAG);
    addEnum(N_INVISFLAG);
    addEnum(N_TRYDROPFLAG);
    addEnum(N_DROPFLAG);
    addEnum(N_SCOREFLAG);
    addEnum(N_INITFLAGS);
    addEnum(N_SAYTEAM);
    addEnum(N_CLIENT);
    addEnum(N_AUTHTRY);
    addEnum(N_AUTHKICK);
    addEnum(N_AUTHCHAL);
    addEnum(N_AUTHANS);
    addEnum(N_REQAUTH);
    addEnum(N_PAUSEGAME);
    addEnum(N_GAMESPEED);
    addEnum(N_ADDBOT);
    addEnum(N_DELBOT);
    addEnum(N_INITAI);
    addEnum(N_FROMAI);
    addEnum(N_BOTLIMIT);
    addEnum(N_BOTBALANCE);
    addEnum(N_MAPCRC);
    addEnum(N_CHECKMAPS);
    addEnum(N_SWITCHNAME);
    addEnum(N_SWITCHMODEL);
    addEnum(N_SWITCHTEAM);
    addEnum(N_INITTOKENS);
    addEnum(N_TAKETOKEN);
    addEnum(N_EXPIRETOKENS);
    addEnum(N_DROPTOKENS);
    addEnum(N_DEPOSITTOKENS);
    addEnum(N_STEALTOKENS);
    addEnum(N_SERVCMD);
    addEnum(N_DEMOPACKET);
    addEnum(NUMMSG);
    addEnum(SAUERBRATEN_LANINFO_PORT);
    addEnum(SAUERBRATEN_SERVER_PORT);
    addEnum(SAUERBRATEN_SERVINFO_PORT);
    addEnum(SAUERBRATEN_MASTER_PORT);
    addEnum(PROTOCOL_VERSION);
    addEnum(DEMO_VERSION);
    lua_pushstring(L, "DEMO_MAGIC"); lua_pushstring(L, DEMO_MAGIC); lua_rawset(L, -3);
    addEnum(MAXNAMELEN);
    addEnum(MAXTEAMLEN);
    addEnum(MAXRAYS);
    addEnum(EXP_SELFDAMDIV);
    addEnum(EXP_SELFPUSH);
    addEnum(EXP_DISTSCALE);
    lua_pushstring(L, "itemstats"); push(L, lua_arrayproxy<decltype(itemstats)>(itemstats)); lua_rawset(L, -3);
    lua_pushstring(L, "guns"); push(L, lua_arrayproxy<decltype(guns)>(guns)); lua_rawset(L, -3);
    addEnum(EXT_ACK);
    addEnum(EXT_VERSION);
    addEnum(EXT_NO_ERROR);
    addEnum(EXT_ERROR);
    addEnum(EXT_PLAYERSTATS_RESP_IDS);
    addEnum(EXT_PLAYERSTATS_RESP_STATS);
    addEnum(EXT_UPTIME);
    addEnum(EXT_PLAYERSTATS);
    addEnum(EXT_TEAMSCORE);
    addEnum(AI_NONE);
    addEnum(AI_BOT);
    addEnum(AI_MAX);
    addEnum(MAXTEAMS);
    lua_pushstring(L, "clientinfo"); lua_rawget(L, -2); lua_pushstring(L, "PUSHMILLIS"); lua_pushnumber(L, clientinfo::PUSHMILLIS); lua_rawset(L, -3); lua_pop(L, 1);
#define addPtr(n) lua_pushstring(L, #n); push(L, &n); lua_rawset(L, -3)
    addPtr(connects);
    addPtr(clients);
    addPtr(bots);
    addPtr(ments);
    addPtr(sents);
    addPtr(scores);
    addPtr(capturemode);
    addPtr(ctfmode);
    addPtr(collectmode);
#undef addPtr
#undef addEnum
    lua_pop(L, 1);
}

}
