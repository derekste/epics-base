/*************************************************************************\
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include <osiSock.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__sun)
#  include <resolv.h>
#  undef res_query
#endif

int epicsStdCall asTestResolveHag(const char *, unsigned short, struct sockaddr_in *);
int asTestQueryHag(const char *, int, int, unsigned char *, int);

#define aToIPAddr asTestResolveHag
#define res_query asTestQueryHag
#include "asLib.c"
