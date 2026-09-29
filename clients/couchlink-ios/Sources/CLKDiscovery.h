//
//  CLKDiscovery.h
//  Couchlink
//
//  Finds couchlink-host on the local network: the PC announces itself as a
//  Bonjour service of type _couchlink._udp. Main thread only.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface CLKDiscoveredPC : NSObject
/// The name the PC gives itself, the same one it reports when Couchlink connects.
@property (nonatomic, copy, readonly) NSString *name;
/// Ready for -[CLKBridge connectToPC:]: "192.168.1.20", or with ":port" if not the default.
@property (nonatomic, copy, readonly) NSString *address;
@end

@class CLKDiscovery;

@protocol CLKDiscoveryDelegate <NSObject>
/// The list of PCs changed. Main thread.
- (void)discoveryDidChange:(CLKDiscovery *)discovery;
/// Browsing failed, for example because Local Network access is off. Main thread.
- (void)discovery:(CLKDiscovery *)discovery didFailWithCode:(NSInteger)code;
@end

@interface CLKDiscovery : NSObject

- (instancetype)initWithDelegate:(id<CLKDiscoveryDelegate>)delegate;
- (instancetype)init NS_UNAVAILABLE;

/// Browse until -stop. Browsing only works while the app is in the foreground.
- (void)start;
- (void)stop;

/// Resolved PCs, sorted by name.
@property (nonatomic, readonly) NSArray<CLKDiscoveredPC *> *pcs;

@end

NS_ASSUME_NONNULL_END
