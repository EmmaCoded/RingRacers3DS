// rr_nete.c - Ring Racers E (PC), . Ver rr_nete.h.
//
// ONLINE ONLY BETWEEN E BUILDS: the E client sends application = RR_NETAPP ("RingRacersE"); the E server rejects
// anyone else with "Ring Racers E only". The E server announces application "RingRacers" + the SV_RRE bit,
// so the original Ring Racers can request to join and receives that notice. The E client requests the SV_RRE bit.
// No public list: only "Direct Join" by IP (the list would be the original's, incompatible).
//
// LOW-END OVER THE NETWORK (changes the simulation -> everyone must use the same):
// - netvar rrnetlowend = the match's low-end setting. Everyone reads it in P_LoadLevel. The server only changes it
// OUTSIDE a map (intermission/voting), so a whole map always uses the same value.
// - The server wants low-end if the host has it on OR any low-end player is connected.
// - A low-end client joining a non-low-end match: becomes a spectator (bit in the netvar
// rrnetlowwait, which K_CheckSpectateStatus honors identically on all machines) until the round
// ends and the server switches to low-end. If they leave, when the round ends the host's option returns.
// - Normal client joining a low-end match: plays in low-end (notice).
#include "doomdef.h"
#include "doomstat.h"
#include "d_clisrv.h"
#include "d_net.h"
#include "d_netcmd.h"
#include "command.h"
#include "console.h"
#include "g_game.h"
#include "k_menu.h"
#include "s_sound.h"
#include "hu_stuff.h" // chat_on
#include "screen.h"
#include "rr_nete.h"
#include "k_hud.h" // K_DrawGameControl
#include "v_video.h"

#ifdef RR_PCGEN

static UINT8 s_nodelowend[MAXNETNODES]; // server: the client of that node requested low-end
static int s_wrongserver;   // client: 1 = original Ring Racers, 2 = another game/mod, 3 = another version of E
static int s_joinmsg;       // client: 1 = I am low-end and the host is not, 2 = the host is low-end and I am not
static int s_sentlow = -1;  // server: rrnetlowend value already requested (waiting for the XD to arrive)
static int s_sentwait = -1; // server: same as rrnetlowwait

// ------------------------------------------------------------------ client

void RRNet_CheckServerInfo(const serverinfo_pak *info)
{
	if (strcmp(info->application, SRB2APPLICATION))
		s_wrongserver = 2;
	else if (!(info->kartvars & SV_RRE))
		s_wrongserver = 1;
	else if (info->version != VERSION || info->subversion != RR_NETINFO_SUBVERSION)
		s_wrongserver = 3;
	else
		s_wrongserver = 0;
}

int RRNet_WrongServer(void)
{
	return s_wrongserver != 0;
}

void RRNet_ShowWrongServer(void)
{
	const char *txt;
	if (s_wrongserver == 1)
		txt = "That is an original Ring Racers game.\n\nRing Racers E can only join\nRing Racers E games (its files and\ngame logic differ).\n";
	else if (s_wrongserver == 3)
		txt = "That Ring Racers E game uses a\ndifferent version.\n\nBoth players need the same\nRing Racers E version.\n";
	else
		txt = "That is not a Ring Racers E game.\n\nRing Racers E can only join\nRing Racers E games.\n";
	CONS_Printf("[RRNET] incompatible server (type %d): not Ring Racers E of the same version\n", s_wrongserver);
	s_wrongserver = 0;
	M_StartMessage("Server Connection Failure", M_GetText(txt), NULL, MM_NOTHING, NULL, "Back to Menu");
}

void RRNet_ClientFoundServer(int serverlowend)
{
	if (cv_rrlowend.value && !serverlowend)
		s_joinmsg = 1;
	else if (!cv_rrlowend.value && serverlowend)
		s_joinmsg = 2;
	else
		s_joinmsg = 0;
}

static void RRNet_ClientUpdate(void)
{
	if (!netgame)
	{
		s_joinmsg = 0;
		return;
	}
	if (!s_joinmsg || !addedtogame || menuactive || chat_on)
		return;
	if (gamestate != GS_LEVEL && gamestate != GS_INTERMISSION && gamestate != GS_VOTING)
		return;

	if ((s_joinmsg == 1 && !cv_rrnetlowend.value) || (s_joinmsg == 2 && cv_rrnetlowend.value))
		M_StartControlPanel(); // the banner is only drawn with the menu open (online it does not pause)
	if (s_joinmsg == 1 && !cv_rrnetlowend.value)
		M_StartMessage("Low-End Mode", M_GetText("The host isn't using Low-End Mode.\n\nYou'll spectate until this round\nends. Then everyone switches to\nLow-End Mode and you can race.\n"), NULL, MM_NOTHING, NULL, NULL);
	else if (s_joinmsg == 2 && cv_rrnetlowend.value)
		M_StartMessage("Low-End Mode", M_GetText("This game uses Low-End Mode,\nso you'll play with it too.\n(The host decides.)\n"), NULL, MM_NOTHING, NULL, NULL);
	s_joinmsg = 0;
}

// ------------------------------------------------------------------ server

void RRNet_SetNodeLowEnd(int node, int lowend)
{
	if (node < 0 || node >= MAXNETNODES)
		return;
	if (server && netgame && lowend && !s_nodelowend[node] && !cv_rrnetlowend.value)
		CONS_Printf("\x82" "A Low-End Mode player is joining: everyone switches to Low-End Mode when this round ends.\n");
	s_nodelowend[node] = lowend ? 1 : 0;
}

static int RRNet_PlayerLowEnd(int i)
{
	if (!playeringame[i] || players[i].bot)
		return 0;
	if (playernode[i] >= MAXNETNODES)
		return 0;
	return s_nodelowend[playernode[i]];
}

void RRNet_BeforeAddPlayer(int node, int playernum)
{
	int mask, want;
	if (!server || !netgame || playernum < 0 || playernum >= MAXPLAYERS)
		return;
	mask = cv_rrnetlowwait.value;
	if (s_sentwait >= 0)
		mask = s_sentwait; // a change was already on its way: start from that one
	want = (node >= 0 && node < MAXNETNODES && s_nodelowend[node] && !cv_rrnetlowend.value);
	if (want)
		mask |= (1 << playernum);
	else
		mask &= ~(1 << playernum); // that number belonged to a low-end player who has already left
	if (mask != cv_rrnetlowwait.value || (s_sentwait >= 0 && mask != s_sentwait))
	{
		CV_SetValue(&cv_rrnetlowwait, mask);
		s_sentwait = mask;
	}
}

void RRNet_ServerStart(void)
{
	// Nobody is connected yet: it is set directly (no XD, which would be lost before the host joins).
	boolean ng = netgame;
	memset(s_nodelowend, 0, sizeof(s_nodelowend));
	s_sentlow = s_sentwait = -1;
	netgame = false;
	CV_StealthSetValue(&cv_rrnetlowend, cv_rrlowend.value ? 1 : 0);
	CV_StealthSetValue(&cv_rrnetlowwait, 0);
	netgame = ng;
	CONS_Printf("[LOWEND] online game %s\n", cv_rrnetlowend.value ? "with low-end (the host uses it)" : "without low-end");
}

static void RRNet_ServerUpdate(void)
{
	int i, want = cv_rrlowend.value ? 1 : 0, mask = 0;

	for (i = 0; i < MAXPLAYERS; i++)
		if (RRNet_PlayerLowEnd(i))
			want = 1;

	// 1) match low-end: only between rounds (never mid-map)
	if (cv_rrnetlowend.value == want)
		s_sentlow = -1;
	else if (gamestate != GS_LEVEL && s_sentlow != want)
	{
		CV_SetValue(&cv_rrnetlowend, want);
		s_sentlow = want;
	}

	// 2) who waits as a spectator: low-end players while the match is not low-end
	if (!cv_rrnetlowend.value)
		for (i = 0; i < MAXPLAYERS; i++)
			if (RRNet_PlayerLowEnd(i))
				mask |= (1 << i);
	if (cv_rrnetlowwait.value == mask)
		s_sentwait = -1;
	else if (s_sentwait != mask)
	{
		CV_SetValue(&cv_rrnetlowwait, mask);
		s_sentwait = mask;
	}
}

// ------------------------------------------------------------------ both

static void Command_RRMyIP_f(void)
{
	RRNet_ShowMyIPs(0);
}

void RRNet_Update(void)
{
	static int registered = 0;
	if (!registered) // console command "rrmyip" = the same banner as the button
	{
		registered = 1;
		COM_AddCommand("rrmyip", Command_RRMyIP_f);
	}
	if (!netgame)
	{
		s_sentlow = s_sentwait = -1;
		s_joinmsg = 0;
		return;
	}
	if (server)
		RRNet_ServerUpdate();
	else
		RRNet_ClientUpdate();
}

void RRNet_LowEndNetChanged(void)
{
	extern int rr_lowend;
	if (!netgame)
		return;
	// It is applied on the same tic on ALL machines (the XD arrives in order), so whoever joins mid-
	// map (loading the current netvar) always matches the others even if the change lands during a map.
	rr_lowend = cv_rrnetlowend.value ? 1 : 0;
	if (client && !addedtogame) // on join the netvars arrive all at once: it is not a change
		return;
	CONS_Printf("\x82" "Low-End Mode %s from the next course.\n", cv_rrnetlowend.value ? "ON" : "OFF");
}

void RRNet_ShowMyIPs(int choice)
{
	extern const char *I_RR_ServerPort(void);
	static char list[1024];
	static char msg[1400];
	const char *port = I_RR_ServerPort();
	int hidden = 0, n;
	(void)choice;

	n = RRNetIP_List(list, sizeof(list), (port && *port) ? port : NULL, &hidden);
	if (n <= 0)
		snprintf(list, sizeof(list), "(No local network found.)\n");

	snprintf(msg, sizeof(msg),
		"Other Ring Racers E players can\njoin with Online > Direct Join:\n\n%s\n%s"
		"Only Ring Racers E can join.\nYour public IP is never shown.\nTo play over the internet like a LAN,\nuse ZeroTier (or similar) on all PCs.\n",
		list,
		hidden ? "(A public IP was hidden for safety.)\n" : "");
	CONS_Printf("[RRNET] My IP Addresses:\n%s", list);
	if (!menuactive)
		M_StartControlPanel(); // from the console: the banner is only visible with the menu open
	M_StartMessage("My IP Addresses", msg, NULL, MM_NOTHING, NULL, NULL);
	rrnet_ipmsg_open = 1; // this banner is also closed with Y (message-box.c)
}

// "<Y> My IP Addresses" notice at the bottom left (Online menu and host pause). X does not work: in menus it is "back".
// strip = dark band behind (Online menu, where the master server's used to be).
int rrnet_ipmsg_open = 0;
void RRNet_DrawMyIPsHint(INT32 x, INT32 y, boolean strip)
{
	if (strip)
		V_DrawFadeFill(0, y-1, BASEVIDWIDTH, 10+1, 0, 31, 5);
	K_DrawGameControl(x, y, 0, "<y_animated> My IP Addresses", 0, TINY_FONT, 0);
}

// Y pressed in that menu: opens the IP banner. true = the key was used.
boolean RRNet_MyIPsInput(UINT8 pid)
{
	if (!M_MenuButtonPressed(pid, MBT_Y))
		return false;
	M_SetMenuDelay(pid);
	RRNet_ShowMyIPs(0);
	return true;
}

#endif // RR_PCGEN
