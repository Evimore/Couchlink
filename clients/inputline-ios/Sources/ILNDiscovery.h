//
//  ILNDiscovery.h
//  InputLine
//
//  Finds inputline-host on the local network: the PC announces itself as a
//  Bonjour service of type _inputline._udp. Main thread only.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface ILNDiscoveredPC : NSObject
/// The name the PC gives itself, the same one it reports when InputLine connects.
@property (nonatomic, copy, readonly) NSString *name;
/// Ready for -[ILNBridge connectToPC:]: "192.168.1.20", or with ":port" if not the default.
@property (nonatomic, copy, readonly) NSString *address;
/// For screenshots of the app.
+ (instancetype)pcWithName:(NSString *)name address:(NSString *)address;
@end

@class ILNDiscovery;

@protocol ILNDiscoveryDelegate <NSObject>
/// The list of PCs changed. Main thread.
- (void)discoveryDidChange:(ILNDiscovery *)discovery;
/// Browsing failed, for example because Local Network access is off. Main thread.
- (void)discovery:(ILNDiscovery *)discovery didFailWithCode:(NSInteger)code;
@end

@interface ILNDiscovery : NSObject

- (instancetype)initWithDelegate:(id<ILNDiscoveryDelegate>)delegate;
- (instancetype)init NS_UNAVAILABLE;

/// Browse until -stop. Browsing only works while the app is in the foreground.
- (void)start;
- (void)stop;

/// Resolved PCs, sorted by name.
@property (nonatomic, readonly) NSArray<ILNDiscoveredPC *> *pcs;

@end

NS_ASSUME_NONNULL_END
