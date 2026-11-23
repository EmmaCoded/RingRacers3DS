// rr_nete.h - Ring Racers E (PC): online ONLY between Ring Racers E builds (direct connection by IP),
// "My IP Addresses" button and network-synchronized low-end mode. Code in rr_nete.c and rr_netip.cpp.
#ifndef RR_NETE_H
#define RR_NETE_H

#ifdef RR_PCGEN

#ifdef __cplusplus
extern "C" {
#endif

struct serverinfo_pak;

// --- client ---
void RRNet_CheckServerInfo(const struct serverinfo_pak *info); // when the info of the server we are joining arrives
int RRNet_WrongServer(void);       // 1 if that server is not Ring Racers E (or is another version)
void RRNet_ShowWrongServer(void);  // "Ring Racers E only" message (and clears the notice)
void RRNet_ClientFoundServer(int serverlowend); // prepares the low-end mismatch notice

// --- server ---
void RRNet_SetNodeLowEnd(int node, int lowend); // the client of that node uses low-end
void RRNet_BeforeAddPlayer(int node, int playernum); // before XD_ADDPLAYER: flag waiting if needed
void RRNet_ServerStart(void);      // SV_SpawnServer (online): the match starts with the host's low-end setting

// --- both ---
void RRNet_Update(void);           // every NetUpdate
void RRNet_LowEndNetChanged(void); // onchange of the rrnetlowend netvar (runs on all machines)
void RRNet_ShowMyIPs(int choice);  // "My IP Addresses" button (Host menu) and Y key (Online menu and host pause)
void RRNet_DrawMyIPsHint(INT32 x, INT32 y, boolean strip); // "<Y> My IP Addresses" notice at the bottom left
boolean RRNet_MyIPsInput(UINT8 pid);      // Y -> IP banner (true if it was used)
extern int rrnet_ipmsg_open;              // the open banner is the IP one (also closed with Y)

// rr_netip.cpp: writes into out the useful local IPs (LAN, Wi-Fi, ZeroTier, Hamachi...) with the port,
// one per line. NEVER the public IP. Returns how many it wrote; *hidden = how many public ones were hidden.
int RRNetIP_List(char *out, int outsz, const char *port, int *hidden);

#ifdef __cplusplus
}
#endif

#endif // RR_PCGEN
#endif // RR_NETE_H
