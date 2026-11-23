// The C spellings of the socket calls, which the decomp's curl uses. As in socket.cpp
// there is no network: connections fail and nothing arrives. The parameter lists are
// left open because only the results matter.
int nnsocketGetLastErrno() { return 50; }       /* ENETDOWN */
void nnsocketSetLastErrno() {}
int nnsocketSocket() { return -1; }
int nnsocketClose() { return 0; }
int nnsocketBind() { return -1; }
int nnsocketConnect() { return -1; }
int nnsocketFcntl() { return -1; }
int nnsocketGetPeerName() { return -1; }
int nnsocketGetSockName() { return -1; }
int nnsocketGetSockOpt() { return -1; }
int nnsocketSetSockOpt() { return -1; }
int nnsocketPoll() { return -1; }
long nnsocketRecv() { return -1; }
long nnsocketSend() { return -1; }
void * nnsocketGetHostByNameWithOptions() { return 0; }
int nnsocketResolverGetOption() { return -1; }
int nnsocketResolverSetOption() { return -1; }
