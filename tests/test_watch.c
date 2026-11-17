#define main watcher_main
#include "../native/watch.c"
#undef main
#include <assert.h>

static bool route(unsigned short type, unsigned char family, unsigned char table,
    unsigned char proto, unsigned char scope, unsigned char kind, unsigned char prefix,
    const char *dest, uint32_t ext_table) {
    union {
        struct nlmsghdr align;
        unsigned char data[128];
    } msg = {0};
    struct nlmsghdr *h = (void *)msg.data;
    h->nlmsg_type = type;
    h->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    struct rtmsg *r = NLMSG_DATA(h);
    r->rtm_family = family; r->rtm_table = table; r->rtm_protocol = proto;
    r->rtm_scope = scope; r->rtm_type = kind; r->rtm_dst_len = prefix;
    if (dest) {
        struct rtattr *a = (void *)(msg.data + NLMSG_ALIGN(h->nlmsg_len));
        a->rta_type = RTA_DST; a->rta_len = RTA_LENGTH(4);
        struct in_addr addr;
        assert(inet_pton(AF_INET, dest, &addr) == 1);
        memcpy(RTA_DATA(a), &addr, 4);
        h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + RTA_ALIGN(a->rta_len);
    }
    if (ext_table) {
        struct rtattr *a = (void *)(msg.data + NLMSG_ALIGN(h->nlmsg_len));
        a->rta_type = RTA_TABLE; a->rta_len = RTA_LENGTH(4);
        memcpy(RTA_DATA(a), &ext_table, 4);
        h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + RTA_ALIGN(a->rta_len);
    }
    return relevant_route(h);
}

int main(void) {
    assert(route(RTM_NEWROUTE, AF_INET, 254, 2, RT_SCOPE_LINK, RTN_UNICAST, 24, "169.254.2.0", 0));
    assert(route(RTM_DELROUTE, AF_INET, 254, 2, RT_SCOPE_LINK, RTN_UNICAST, 24, "192.168.1.0", 0));
    assert(route(RTM_NEWROUTE, AF_INET, 0, 2, RT_SCOPE_LINK, RTN_UNICAST, 24, "10.1.0.0", 1004));
    assert(route(RTM_NEWROUTE, AF_INET, 52, 2, RT_SCOPE_UNIVERSE, RTN_UNICAST, 0, NULL, 0));
    assert(route(RTM_DELROUTE, AF_INET, 52, 242, RT_SCOPE_LINK, RTN_UNICAST, 24, "169.254.2.0", 0));
    assert(route(RTM_DELROUTE, AF_INET, 0, 2, RT_SCOPE_UNIVERSE, RTN_THROW, 24, "172.31.0.0", 52));
    assert(!route(RTM_NEWROUTE, AF_INET, 52, 242, RT_SCOPE_LINK, RTN_UNICAST, 24, "169.254.2.0", 0));
    assert(!route(RTM_NEWROUTE, AF_INET, 254, 2, RT_SCOPE_LINK, RTN_UNICAST, 24, "172.32.0.0", 0));
    assert(!route(RTM_NEWROUTE, AF_INET, 254, 2, RT_SCOPE_UNIVERSE, RTN_UNICAST, 0, NULL, 0));
    assert(!route(RTM_NEWROUTE, AF_INET6, 52, 2, RT_SCOPE_LINK, RTN_UNICAST, 0, NULL, 0));
    assert(!route(RTM_NEWROUTE, AF_INET, 255, 2, RT_SCOPE_LINK, RTN_LOCAL, 24, "10.0.0.0", 0));
    assert(ts_name("tailscaled.state") && ts_name("tailscaled.sock") && ts_name("tmp"));
    assert(!ts_name("tailscaled.log"));
    puts("watch event filter tests passed");
    return 0;
}
