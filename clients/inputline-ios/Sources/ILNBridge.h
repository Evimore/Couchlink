//
//  ILNBridge.h
//  InputLine
//
//  Reads the Steam Controller over Bluetooth and forwards its raw reports to
//  inputline-host on the gaming PC, which plugs in a virtual wired Steam
//  Controller. Runs in the background, next to any streaming app.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class ILNDiscoveredPC;

typedef NS_ENUM(NSInteger, ILNLinkState) {
    ILNLinkStateNoPC,        // no PC address yet
    ILNLinkStateSearching,   // probing the PC
    ILNLinkStateNotFound,    // no answer yet; still trying
    ILNLinkStatePairing,     // waiting for the code shown on the PC
    ILNLinkStateConnecting,  // paired; starting a session
    ILNLinkStateConnected,
    ILNLinkStateDisconnected,  // the user tapped Disconnect in the PC section
};

typedef NS_ENUM(NSInteger, ILNControllerState) {
    ILNControllerStateOnPC,          // plugged in on the PC
    ILNControllerStateConnecting,    // being plugged in
    ILNControllerStateWaitingForPC,  // no link to the PC right now
    ILNControllerStateOnThisDevice,  // Disconnect was tapped: works with this device
};

@interface ILNControllerInfo : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic) ILNControllerState state;
@end

/// A snapshot for the UI; safe to read on the main thread.
@interface ILNStatus : NSObject
@property (nonatomic) ILNLinkState linkState;
@property (nonatomic, copy) NSString *pcAddress;
@property (nonatomic, copy) NSString *pcName;
@property (nonatomic, copy) NSString *linkText;
/// Connected, or briefly reconnecting after a drop: the PC section shows Disconnect.
@property (nonatomic) BOOL linkUp;
/// The PC and this app run different versions: which one to update. Empty otherwise.
@property (nonatomic, copy) NSString *updateText;
@property (nonatomic, copy) NSArray<NSString *> *controllers;  // "name: state", for the report
@property (nonatomic, copy) NSArray<ILNControllerInfo *> *controllerInfo;
/// Talking to the PC through Tailscale.
@property (nonatomic) BOOL viaTailscale;
/// Why the PC can't be reached (no answer, version mismatch), when it can't. Empty otherwise.
@property (nonatomic, copy) NSString *problemText;
@property (nonatomic) double rttMs;               // -1 if unknown
/// The user tapped Disconnect: controllers work with this device, not the PC.
@property (nonatomic) BOOL paused;
@property (nonatomic, copy) NSString *timingNow;  // Bluetooth report timing, last few seconds
@property (nonatomic, copy) NSString *timingForeground;
@property (nonatomic, copy) NSString *timingBackground;
@property (nonatomic, copy) NSString *timingSent;       // when reports leave for the PC
/// inputline-host PCs announcing themselves on this network (while the app is in front).
@property (nonatomic, copy) NSArray<ILNDiscoveredPC *> *discoveredPCs;
/// Recent events, oldest first ("12:03:04.123  Controller disconnected: ...").
@property (nonatomic, copy) NSArray<NSString *> *events;
@end

@class ILNBridge;

@protocol ILNBridgeDelegate <NSObject>
/// The PC is showing a pairing code; ask the user for it. Main thread.
- (void)bridge:(ILNBridge *)bridge needsPairingCodeWithMessage:(NSString *)message;
/// Something the user should know (short). Main thread.
- (void)bridge:(ILNBridge *)bridge showMessage:(NSString *)message;
@end

@interface ILNBridge : NSObject

+ (instancetype)shared;

@property (nonatomic, weak, nullable) id<ILNBridgeDelegate> delegate;

/// Start Bluetooth, looking for PCs on the network and, if a PC was saved, the link.
/// Call once at launch, on the main thread.
- (void)start;

/// Save a PC address ("192.168.1.20", "gaming-pc.local", "100.64.1.2", "[fe80::1]:48150") and connect.
- (void)connectToPC:(NSString *)address;

/// The PC section's Disconnect: end the link, unplug the controllers on the
/// PC and give them back to this device. It stays disconnected, also after
/// InputLine restarts, until connectToPC: is called.
- (void)disconnectFromPC;

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

- (ILNStatus *)status;

@end

NS_ASSUME_NONNULL_END
