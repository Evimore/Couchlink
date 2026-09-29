//
//  CLKDiscovery.m
//  Couchlink
//

#import "CLKDiscovery.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

static NSString *const kServiceType = @"_couchlink._udp.";
static const uint16_t kDefaultPort = 48150;
static const NSTimeInterval kResolveTimeout = 5.0;

@interface CLKDiscoveredPC ()
@property (nonatomic, copy, readwrite) NSString *name;
@property (nonatomic, copy, readwrite) NSString *address;
@end

@implementation CLKDiscoveredPC
@end

/// The address to connect to, from a resolved service's socket addresses. Prefers IPv4.
static NSString *_Nullable AddressFromSockaddrs(NSArray<NSData *> *addresses)
{
    NSData *chosen = nil;
    for (NSData *data in addresses) {
        if (data.length < sizeof(struct sockaddr)) {
            continue;
        }
        const struct sockaddr *address = (const struct sockaddr *)data.bytes;
        if (address->sa_family == AF_INET && data.length >= sizeof(struct sockaddr_in)) {
            chosen = data;
            break;
        }
        if (address->sa_family == AF_INET6 && data.length >= sizeof(struct sockaddr_in6) && chosen == nil) {
            chosen = data;
        }
    }
    if (chosen == nil) {
        return nil;
    }

    const struct sockaddr *address = (const struct sockaddr *)chosen.bytes;
    char host[NI_MAXHOST] = {0};
    if (getnameinfo(address, (socklen_t)chosen.length, host, sizeof(host), NULL, 0, NI_NUMERICHOST) != 0) {
        return nil;
    }
    const uint16_t port = address->sa_family == AF_INET ? ntohs(((const struct sockaddr_in *)address)->sin_port)
                                                        : ntohs(((const struct sockaddr_in6 *)address)->sin6_port);
    NSString *text = [NSString stringWithUTF8String:host];
    const BOOL ipv6 = address->sa_family == AF_INET6;
    if (port == kDefaultPort || port == 0) {
        return text;
    }
    return ipv6 ? [NSString stringWithFormat:@"[%@]:%u", text, port] : [NSString stringWithFormat:@"%@:%u", text, port];
}

@interface CLKDiscovery () <NSNetServiceBrowserDelegate, NSNetServiceDelegate>
@end

@implementation CLKDiscovery {
    __weak id<CLKDiscoveryDelegate> _delegate;
    NSNetServiceBrowser *_browser;
    NSMutableArray<NSNetService *> *_services;
    NSMutableDictionary<NSString *, CLKDiscoveredPC *> *_resolved;  // by service name
}

- (instancetype)initWithDelegate:(id<CLKDiscoveryDelegate>)delegate
{
    if ((self = [super init])) {
        _delegate = delegate;
        _services = [NSMutableArray array];
        _resolved = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void)start
{
    if (_browser != nil) {
        return;
    }
    _browser = [[NSNetServiceBrowser alloc] init];
    _browser.delegate = self;
    [_browser searchForServicesOfType:kServiceType inDomain:@"local."];
}

- (void)stop
{
    [_browser stop];
    _browser.delegate = nil;
    _browser = nil;
    for (NSNetService *service in _services) {
        service.delegate = nil;
        [service stop];
    }
    [_services removeAllObjects];
    if (_resolved.count > 0) {
        [_resolved removeAllObjects];
        [_delegate discoveryDidChange:self];
    }
}

- (NSArray<CLKDiscoveredPC *> *)pcs
{
    return [_resolved.allValues sortedArrayUsingComparator:^NSComparisonResult(CLKDiscoveredPC *a, CLKDiscoveredPC *b) {
        return [a.name localizedCaseInsensitiveCompare:b.name];
    }];
}

#pragma mark - NSNetServiceBrowserDelegate

- (void)netServiceBrowser:(NSNetServiceBrowser *)browser didFindService:(NSNetService *)service moreComing:(BOOL)moreComing
{
    [_services addObject:service];
    service.delegate = self;
    [service resolveWithTimeout:kResolveTimeout];
}

- (void)netServiceBrowser:(NSNetServiceBrowser *)browser didRemoveService:(NSNetService *)service moreComing:(BOOL)moreComing
{
    for (NSNetService *known in [_services copy]) {
        if ([known.name isEqualToString:service.name]) {
            known.delegate = nil;
            [known stop];
            [_services removeObject:known];
        }
    }
    if (_resolved[service.name] != nil) {
        [_resolved removeObjectForKey:service.name];
        [_delegate discoveryDidChange:self];
    }
}

- (void)netServiceBrowser:(NSNetServiceBrowser *)browser didNotSearch:(NSDictionary<NSString *, NSNumber *> *)errorDict
{
    [_delegate discovery:self didFailWithCode:errorDict[NSNetServicesErrorCode].integerValue];
    // Let a later -start try again.
    _browser.delegate = nil;
    _browser = nil;
}

#pragma mark - NSNetServiceDelegate

- (void)netServiceDidResolveAddress:(NSNetService *)service
{
    NSString *address = AddressFromSockaddrs(service.addresses ?: @[]);
    if (address == nil) {
        return;
    }
    CLKDiscoveredPC *pc = [[CLKDiscoveredPC alloc] init];
    pc.name = service.name;
    pc.address = address;
    CLKDiscoveredPC *old = _resolved[service.name];
    if (old != nil && [old.address isEqualToString:address]) {
        return;
    }
    _resolved[service.name] = pc;
    [_delegate discoveryDidChange:self];
}

- (void)netService:(NSNetService *)service didNotResolve:(NSDictionary<NSString *, NSNumber *> *)errorDict
{
    // The PC may have just gone away; the browser reports removals separately.
}

@end
