#ifndef UDPDIAG_H
#define UDPDIAG_H

// One-shot network probe: sends a DHT ping to a bootstrap router three ways
// (unconnected+blocking, unconnected+select, connected) and logs which style
// actually receives the reply. Used to find out why DHT/µTP get no inbound UDP.
void udp_diagnose(void (*logfn)(const char *));

#endif
