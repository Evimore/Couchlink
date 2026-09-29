//
//  CLKBridge.h
//  Couchlink
//
//  Reads the Steam Controller over Bluetooth and forwards its raw reports to
//  couchlink-host on the gaming PC, which plugs in a virtual wired Steam
//  Controller. Runs in the background, next to any streaming app.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class CLKDiscoveredPC;

typedef NS_ENUM(NSInteger, CLKLinkState) {
    CLKLinkStateNoPC,        // no PC address yet
    CLKLinkStateSearching,   // probing the PC
    CLKLinkStateNotFound,    // no answer yet; still trying
    CLKLinkStatePairing,     // waiting for the code shown on the PC
    CLKLinkStateConnecting,  // paired; starting a session
    CLKLinkStateConnected,
};

/// A snapshot for the UI; safe to read on the main thread.
@interface CLKStatus : NSObject
@property (nonatomic) CLKLinkState linkState;
@property (nonatomic, copy) NSString *pcAddress;
@property (nonatomic, copy) NSString *pcName;
@property (nonatomic, copy) NSString *linkText;
@property (nonatomic, copy) NSArray<NSString *> *controllers;
@property (nonatomic) double rttMs;               // -1 if unknown
/// The user tapped Disconnect: controllers work with this device, not the PC.
@property (nonatomic) BOOL paused;
@property (nonatomic, copy) NSString *timingNow;  // Bluetooth report timing, last few seconds
@property (nonatomic, copy) NSString *timingForeground;
@property (nonatomic, copy) NSString *timingBackground;
@property (nonatomic, copy) NSString *timingSent;       // when reports leave for the PC
/// couchlink-host PCs announcing themselves on this network (while the app is in front).
@property (nonatomic, copy) NSArray<CLKDiscoveredPC *> *discoveredPCs;
/// Recent events, oldest first ("12:03:04.123  Controller disconnected: ...").
@property (nonatomic, copy) NSArray<NSString *> *events;
@end

@class CLKBridge;

@protocol CLKBridgeDelegate <NSObject>
/// The PC is showing a pairing code; ask the user for it. Main thread.
- (void)bridge:(CLKBridge *)bridge needsPairingCodeWithMessage:(NSString *)message;
/// Something the user should know (short). Main thread.
- (void)bridge:(CLKBridge *)bridge showMessage:(NSString *)message;
@end

@interface CLKBridge : NSObject

+ (instancetype)shared;

@property (nonatomic, weak, nullable) id<CLKBridgeDelegate> delegate;

/// Start Bluetooth, looking for PCs on the network and, if a PC was saved, the link.
/// Call once at launch, on the main thread.
- (void)start;

/// Save a PC address ("192.168.1.20", "gaming-pc.local", "100.64.1.2", "[fe80::1]:48150") and connect.
- (void)connectToPC:(NSString *)address;

/// Disconnect: unplug the controllers on the PC and give them back to this
/// device (its pointer works again). Connect, or switching a controller off
/// and on, sends them to the PC again.
- (void)setPaused:(BOOL)paused;

/// The app is about to be closed: give the controllers their mouse mode back.
- (void)prepareForTermination;

- (void)submitPairingCode:(NSString *)code;
- (void)cancelPairing;

/// Look for controllers in Bluetooth pairing mode for 30 s (only works in the foreground).
- (void)scanForNewControllers;

- (void)resetTiming;

/// Everything worth sending when reporting a problem: status, timing and events.
- (NSString *)report;

- (CLKStatus *)status;

@end

NS_ASSUME_NONNULL_END
