#define LCB_BOOTSTRAP_DEFINE_STRUCT

#include "iotests.h"
#include "config.h"
#include "internal.h"
#include "bucketconfig/clconfig.h"
#include <lcbio/iotable.h>
#include <set>

using namespace lcb::clconfig;

namespace {
class ConfmonTest : public ::testing::Test
{
    void SetUp() {
        MockEnvironment::Reset();
    }
};
}

struct evstop_listener : Listener {
    lcbio_pTABLE io;
    int called;

    void clconfig_lsn(EventType event, ConfigInfo*) {
        if (event != CLCONFIG_EVENT_GOT_NEW_CONFIG) {
            return;
        }
        called = 1;
        IOT_STOP(io);
    }

    evstop_listener() : Listener(), io(NULL), called(0) {
    }
};

extern "C" {
static void listen_callback1(clconfig_listener *lsn, clconfig_event_t event,
                             clconfig_info *info)
{
}
}

TEST_F(ConfmonTest, testBasic)
{
    HandleWrap hw;
    lcb_t instance;
    MockEnvironment::getInstance()->createConnection(hw, instance);


    lcb_confmon *mon = new Confmon(instance->settings, instance->iotable);
    clconfig_provider *http = mon->get_provider(CLCONFIG_HTTP);
    lcb_clconfig_http_enable(http);
    http->configure_nodes(*instance->ht_nodes);

    mon->prepare();

    EXPECT_EQ(NULL, mon->get_config());
    mon->start();
    mon->start(); // Twice!
    mon->stop();
    mon->stop();

    // Try to find a provider..
    clconfig_provider *provider = mon->get_provider(CLCONFIG_HTTP);
    ASSERT_NE(0, provider->enabled);

    evstop_listener listener;
    listener.io = instance->iotable;
    mon->add_listener(&listener);
    mon->start();
    IOT_START(instance->iotable);
    ASSERT_NE(0, listener.called);
    delete mon;
}


struct listener2 : Listener {
    int call_count;
    lcbio_pTABLE io;
    clconfig_method_t last_source;
    std::set<clconfig_event_t> expected_events;

    void reset() {
        call_count = 0;
        last_source = CLCONFIG_PHONY;
        expected_events.clear();
    }

    listener2() : Listener() {
        io = NULL;
        reset();
    }

    void clconfig_lsn(EventType event, ConfigInfo *info) {
        if (event == CLCONFIG_EVENT_MONITOR_STOPPED) {
            IOT_START(io);
            return;
        }

        if (!expected_events.empty()) {
            if (expected_events.end() ==
                expected_events.find(event)) {
                return;
            }
        }

        call_count++;
        last_source = info->get_origin();
        IOT_STOP(io);

    }
};

static void runConfmonTest(lcbio_pTABLE io, lcb_confmon *mon)
{
    IOT_START(io);
}

TEST_F(ConfmonTest, testCycle)
{
    HandleWrap hw;
    lcb_t instance;
    lcb_create_st cropts;
    MockEnvironment *mock = MockEnvironment::getInstance();

    if (mock->isRealCluster()) {
        return;
    }

    mock->createConnection(hw, instance);
    instance->settings->bc_http_stream_time = 100000;
    instance->memd_sockpool->tmoidle = 100000;

    lcb_confmon *mon = new Confmon(instance->settings, instance->iotable);

    struct listener2 lsn;
    lsn.io = instance->iotable;
    lsn.reset();
    mon->add_listener(&lsn);

    mock->makeConnectParams(cropts, NULL);
    clconfig_provider *cccp = mon->get_provider(CLCONFIG_CCCP);
    clconfig_provider *http = mon->get_provider(CLCONFIG_HTTP);

    hostlist_t hl = hostlist_create();
    hostlist_add_stringz(hl, cropts.v.v2.mchosts, 11210);
    lcb_clconfig_cccp_enable(cccp, instance);
    cccp->configure_nodes(*hl);

    lcb_clconfig_http_enable(http);
    http->configure_nodes(*instance->ht_nodes);
    hostlist_destroy(hl);

    mon->prepare();
    mon->start();
    lsn.expected_events.insert(CLCONFIG_EVENT_GOT_NEW_CONFIG);
    runConfmonTest(lsn.io, mon);

    // Ensure CCCP is functioning properly and we're called only once.
    ASSERT_EQ(1, lsn.call_count);
    ASSERT_EQ(CLCONFIG_CCCP, lsn.last_source);

    mon->start();
    lsn.reset();
    lsn.expected_events.insert(CLCONFIG_EVENT_GOT_ANY_CONFIG);
    runConfmonTest(lsn.io, mon);
    ASSERT_EQ(1, lsn.call_count);
    ASSERT_EQ(CLCONFIG_CCCP, lsn.last_source);

    mock->setCCCP(false);
    mock->failoverNode(5);
    lsn.reset();
    mon->start();
    lsn.expected_events.insert(CLCONFIG_EVENT_GOT_ANY_CONFIG);
    lsn.expected_events.insert(CLCONFIG_EVENT_GOT_NEW_CONFIG);
    runConfmonTest(lsn.io, mon);
    ASSERT_EQ(CLCONFIG_HTTP, lsn.last_source);
    ASSERT_EQ(1, lsn.call_count);
    delete mon;
}

TEST_F(ConfmonTest, testBootstrapMethods)
{
    lcb_t instance;
    HandleWrap hw;
    MockEnvironment::getInstance()->createConnection(hw, instance);
    lcb_error_t err = lcb_connect(instance);
    ASSERT_EQ(LCB_SUCCESS, err);

    // Try the various bootstrap times
    struct lcb_BOOTSTRAP *bs = instance->bootstrap;
    hrtime_t last = bs->last_refresh, cur = 0;

    // Reset it for the time being
    bs->last_refresh = 0;
    instance->confmon->stop();

    // Refreshing now should work
    lcb_bootstrap_common(instance, LCB_BS_REFRESH_THROTTLE);
    ASSERT_TRUE(instance->confmon->is_refreshing());

    cur = bs->last_refresh;
    ASSERT_GT(cur, 0);
    ASSERT_EQ(0, bs->errcounter);
    last = cur;

    // Stop it, so the state is reset
    instance->confmon->stop();
    ASSERT_FALSE(instance->confmon->is_refreshing());

    lcb_bootstrap_common(instance, LCB_BS_REFRESH_THROTTLE|LCB_BS_REFRESH_INCRERR);
    ASSERT_EQ(last, bs->last_refresh);
    ASSERT_EQ(1, bs->errcounter);

    // Ensure that a throttled-without-incr doesn't actually incr
    lcb_bootstrap_common(instance, LCB_BS_REFRESH_THROTTLE);
    ASSERT_EQ(1, bs->errcounter);

    // No refresh yet
    ASSERT_FALSE(instance->confmon->is_refreshing());

    lcb_bootstrap_common(instance, LCB_BS_REFRESH_ALWAYS);
    ASSERT_TRUE(instance->confmon->is_refreshing());
    instance->confmon->stop();
}
