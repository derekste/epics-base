/*************************************************************************\
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <testMain.h>
#include <epicsUnitTest.h>
#include <epicsEvent.h>
#include <epicsThread.h>
#include <epicsTypes.h>
#include <errlog.h>
#include <osiSock.h>
#include <asLib.h>

static const char hagPolicy[] =
    "HAG(slow) {delayed.hag.test}\n"
    "ASG(DEFAULT) { RULE(0, READ) { HAG(slow) } }\n";
static epicsEventId resolverEntered, resolverReleased;
static int delayDNS;
static epicsUInt32 resolvedAddress;
static int exerciseCallbackAPIs;
static FILE *callbackFile;
static struct {
    unsigned calls;
    int inputCalled, independentRegistrations;
    long initialize, initFile, initFP, initMem, refresh;
} callbackProbe;

int epicsStdCall asTestResolveHag(const char *host, unsigned short port, struct sockaddr_in *addr)
{
    if(strcmp(host, "delayed.hag.test"))
        return aToIPAddr(host, port, addr);
    if(delayDNS) {
        epicsEventSignal(resolverEntered);
        if(epicsEventWaitWithTimeout(resolverReleased, 3.0)!=epicsEventWaitOK)
            return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = htonl(resolvedAddress);
    return 0;
}

int asTestQueryHag(const char *host, int dnsClass, int type, unsigned char *answer, int size)
{
    /* Exercise the normal TTL fallback without contacting a DNS server. */
    (void)host; (void)dnsClass; (void)type; (void)answer; (void)size;
    return -1;
}

typedef struct {
    const char *policy;
    int refresh;
    long result;
    unsigned changed;
    epicsEventId started, done;
} CONTROLWORK;

static void controlWork(void *raw)
{
    CONTROLWORK *work = raw;
    epicsEventSignal(work->started);
    work->result = work->refresh ? asRefreshHag(&work->changed) : asInitMem(work->policy, NULL);
    epicsEventSignal(work->done);
}

static int callbackInput(char *buffer, int size)
{
    (void)buffer; (void)size;
    callbackProbe.inputCalled = 1;
    return 0;
}

static void countCallback(ASCLIENTPVT client, asClientStatus status)
{
    unsigned *count = asGetClientPvt(client);
    if(status==asClientCOAR && count) (*count)++;
    if(exerciseCallbackAPIs) {
        ASMEMBERPVT recursiveMember = NULL;
        ASCLIENTPVT recursiveClient = NULL;
        char host[] = "127.0.0.1";
        int ok;

        callbackProbe.calls++;
        callbackProbe.initialize = asInitialize(callbackInput);
        callbackProbe.initFile = asInitFile("/dev/null", NULL);
        callbackProbe.initFP = asInitFP(callbackFile, NULL);
        callbackProbe.initMem = asInitMem(hagPolicy, NULL);
        callbackProbe.refresh = asRefreshHag(NULL);
        ok = (asCheckGet(client) || !asCheckPut(client)) &&
            asAddMember(&recursiveMember, "DEFAULT")==0 &&
            asAddClient(&recursiveClient, recursiveMember, 0, "testing", host)==0;
        if(recursiveClient) ok = asRemoveClient(&recursiveClient)==0 && ok;
        if(recursiveMember) ok = asRemoveMember(&recursiveMember)==0 && ok;
        callbackProbe.independentRegistrations = callbackProbe.independentRegistrations && ok;
    }
}

static unsigned rights(ASCLIENTPVT client)
{
    return (asCheckGet(client) ? 1u : 0u) | (asCheckPut(client) ? 2u : 0u);
}

typedef struct {
    ASMEMBERPVT member, retiredMember;
    ASCLIENTPVT client, retiredClient;
    char host[16];
    int ok;
    unsigned before, callbacks;
    epicsEventId done;
} CLIENTWORK;

static void clientWork(void *raw)
{
    CLIENTWORK *work = raw;
    work->ok = asAddMember(&work->member, "DEFAULT")==0 &&
        asAddClient(&work->client, work->member, 0, "testing", work->host)==0;
    if(work->ok) {
        asPutClientPvt(work->client, &work->callbacks);
        work->ok = asRegisterClientCallback(work->client, countCallback)==0;
        work->before = rights(work->client);
    }
    work->ok = asRemoveClient(&work->retiredClient)==0 && work->ok;
    work->ok = asRemoveMember(&work->retiredMember)==0 && work->ok;
    epicsEventSignal(work->done);
}

static void waitDone(epicsEventId event)
{
    if(epicsEventWaitWithTimeout(event, 3.0)!=epicsEventWaitOK)
        testAbort("concurrent access-library operation did not finish");
}

static HAGNAME *firstHagName(void)
{
    HAG *hag = (HAG *)ellFirst(&((ASBASE *)pasbase)->hagList);
    return hag ? (HAGNAME *)ellFirst(&hag->list) : NULL;
}

static void testSlowControl(int refresh, int callbackAPIs)
{
    ASMEMBERPVT member = NULL;
    ASCLIENTPVT oldClient = NULL, laterClient = NULL;
    ASBASE *before;
    HAGNAME *entry;
    unsigned oldCallbacks = 0, laterCallbacks = 0;
    char oldHost[] = "127.0.0.1", laterHost[] = "127.0.0.2";
    CONTROLWORK control = {0}, competing = {0};
    CLIENTWORK clients = {0};
    int responsive;

    testDiag("client access during slow HAG %s", refresh ? "refresh" : "policy activation");
    delayDNS = 0;
    resolvedAddress = 0x7f000001u;
    asCheckClientIP = 1;
    testOk1(asInitMem(refresh ? hagPolicy : "ASG(DEFAULT) { RULE(0, WRITE) }\n", NULL)==0);
    strcpy(clients.host, "127.0.0.1");
    testOk1(asAddMember(&member, "DEFAULT")==0);
    testOk1(asAddClient(&oldClient, member, 0, "testing", oldHost)==0);
    testOk1(asAddClient(&laterClient, member, 0, "testing", laterHost)==0);
    testOk1(asAddMember(&clients.retiredMember, "DEFAULT")==0);
    testOk1(asAddClient(&clients.retiredClient, clients.retiredMember, 0, "testing", clients.host)==0);
    asPutClientPvt(oldClient, &oldCallbacks);
    asPutClientPvt(laterClient, &laterCallbacks);
    testOk(asRegisterClientCallback(oldClient, countCallback)==0 && oldCallbacks==1,
        "original-address callback registered once");
    testOk(asRegisterClientCallback(laterClient, countCallback)==0 && laterCallbacks==1,
        "later-address callback registered once");
    before = (ASBASE *)pasbase;
    if(refresh) {
        entry = firstHagName();
        entry->expires = 0;
        before->hagExpires = 1;
        resolvedAddress = 0x7f000002u;
    }

    resolverEntered = epicsEventMustCreate(epicsEventEmpty);
    resolverReleased = epicsEventMustCreate(epicsEventEmpty);
    control.started = epicsEventMustCreate(epicsEventEmpty);
    control.done = epicsEventMustCreate(epicsEventEmpty);
    control.policy = hagPolicy;
    control.refresh = refresh;
    delayDNS = 1;
    epicsThreadMustCreate("as-control", epicsThreadPriorityMedium,
        epicsThreadGetStackSize(epicsThreadStackSmall), controlWork, &control);
    waitDone(control.started);
    testOk(epicsEventWaitWithTimeout(resolverEntered, 3.0)==epicsEventWaitOK,
        "real HAG resolver path is paused");
    testOk(rights(oldClient)==(refresh ? 1u : 3u) && (ASBASE *)pasbase==before,
        "complete original policy/cache remains active throughout DNS");
    exerciseCallbackAPIs = callbackAPIs;

    if(!refresh) {
        /* The input buffer is global. A second public loader must not replace
         * it while the first parser is resolving a HAG hostname. */
        competing.started = epicsEventMustCreate(epicsEventEmpty);
        competing.done = epicsEventMustCreate(epicsEventEmpty);
        competing.policy = "ASG(";
        epicsThreadMustCreate("as-competing", epicsThreadPriorityMedium,
            epicsThreadGetStackSize(epicsThreadStackSmall), controlWork, &competing);
        waitDone(competing.started);
        testOk(epicsEventWaitWithTimeout(competing.done, .1)==epicsEventWaitTimeout,
            "competing public policy loaders stay serialized");
    }

    clients.done = epicsEventMustCreate(epicsEventEmpty);
    epicsThreadMustCreate("as-client", epicsThreadPriorityMedium,
        epicsThreadGetStackSize(epicsThreadStackSmall), clientWork, &clients);
    responsive = epicsEventWaitWithTimeout(clients.done, .3)==epicsEventWaitOK;
    epicsEventSignal(resolverReleased);
    waitDone(control.done);
    if(!responsive) waitDone(clients.done);
    if(!refresh) {
        waitDone(competing.done);
        testOk(competing.result==S_asLib_badConfig,
            "competing malformed policy is rejected without disturbing the staged policy");
        testOk((ASBASE *)pasbase!=before, "successful first policy remains active after rejected loader");
    }
    delayDNS = 0;
    exerciseCallbackAPIs = 0;
    testOk(responsive, "member/client creation and removal finish while HAG DNS is paused");
    testOk(clients.ok, "concurrent member/client operations succeed");
    testOk(clients.before==(refresh ? 1u : 3u), "new client uses original rights until cutover");
    testOk(control.result==0 && (!refresh || control.changed==1), "staged policy/cache commits successfully");
    testOk(rights(oldClient)==(refresh ? 0u : 1u), "original-address client gets committed rights");
    testOk(rights(laterClient)==(refresh ? 1u : 0u), "later-address client gets committed rights");
    testOk(rights(clients.client)==(refresh ? 0u : 1u), "client created during DNS participates in cutover");
    testOk(oldCallbacks==2 && laterCallbacks==2 && clients.callbacks==2,
        "all persistent clients receive exactly one rights-transition callback");
    entry = firstHagName();
    testOk(entry && entry->source && !strcmp(entry->source, "delayed.hag.test") &&
        entry->resolved && entry->expires > time(NULL), "committed cache preserves source and TTL retry semantics");
    testOk(asRemoveClient(&oldClient)==0 && asRemoveClient(&laterClient)==0 &&
        asRemoveClient(&clients.client)==0 && asRemoveMember(&member)==0 &&
        asRemoveMember(&clients.member)==0, "all surviving clients/members can be removed after cutover");

    epicsEventDestroy(resolverEntered); epicsEventDestroy(resolverReleased);
    epicsEventDestroy(control.started); epicsEventDestroy(control.done);
    epicsEventDestroy(clients.done);
    if(!refresh) {
        epicsEventDestroy(competing.started); epicsEventDestroy(competing.done);
    }
}

MAIN(asHagConcurrencyTest)
{
    testPlan(75);
    eltc(0);
    testSlowControl(0, 0);
    testSlowControl(1, 0);
    callbackFile = tmpfile();
    testOk1(callbackFile!=NULL);
    if(callbackFile) {
        fputs(hagPolicy, callbackFile);
        rewind(callbackFile);
    }
    callbackProbe.independentRegistrations = 1;
    testSlowControl(0, 1);
    testOk(callbackProbe.calls > 0, "callback-control guard exercised while another publisher owns the control lock");
    testOk(callbackProbe.initialize==S_asLib_InitFailed, "asInitialize rejects synchronous rights-callback calls");
    testOk(callbackProbe.initFile==S_asLib_InitFailed, "asInitFile rejects synchronous rights-callback calls");
    testOk(callbackProbe.initFP==S_asLib_InitFailed, "asInitFP rejects synchronous rights-callback calls");
    testOk(callbackProbe.initMem==S_asLib_InitFailed, "asInitMem rejects synchronous rights-callback calls");
    testOk(callbackProbe.refresh==S_asLib_InitFailed, "asRefreshHag rejects synchronous rights-callback calls");
    testOk(!callbackProbe.inputCalled, "rejected initialization never enters the input callback");
    testOk(callbackProbe.independentRegistrations,
        "cached-rights queries and independent add/remove registrations work from rights callbacks");
    if(callbackFile) fclose(callbackFile);
    eltc(1);
    errlogFlush();
    return testDone();
}
