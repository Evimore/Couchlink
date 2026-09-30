//
//  ILNTritonBLE.h
//  InputLine
//
//  Talks to the 2026 Steam Controller ("Triton") over Bluetooth LE using
//  Valve's GATT service, the same way Steam Link and SDL do: one notifying
//  characteristic carries state reports (0x45 or 0x47), one read/write
//  characteristic carries feature reports, and each output report (haptics)
//  has its own writable characteristic.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class ILNTritonDevice;

@protocol ILNTritonBLEDelegate <NSObject>

/// The controller is connected and streaming input reports.
- (void)tritonDidBecomeReady:(ILNTritonDevice *)device;

/// The controller went away (switched off, out of range, or stop was called).
- (void)tritonDidDisconnect:(ILNTritonDevice *)device;

/// One input report, report ID first. Called on the Bluetooth queue at up
/// to ~250 Hz, so do as little as possible here.
- (void)triton:(ILNTritonDevice *)device didReceiveReport:(const uint8_t *)report length:(size_t)length;

@optional

/// The user has not allowed this app to use Bluetooth.
- (void)tritonBluetoothUnauthorized;

/// Connected; setting the controller up until it streams (then
/// tritonDidBecomeReady:). Called on the Bluetooth queue.
- (void)tritonWillSetUp:(ILNTritonDevice *)device;

/// Something worth a line in the event log (connecting, setup retries).
/// Called on the Bluetooth queue.
- (void)tritonLog:(NSString *)message;

@end

@interface ILNTritonDevice : NSObject

@property (nonatomic, readonly) NSUUID *identifier;
@property (nonatomic, readonly, copy) NSString *name;
/// 0x45 (older firmware) or 0x47 (newer, with trackpad timestamps).
@property (nonatomic, readonly) uint8_t inputReportId;
/// Why the last connection ended, as CoreBluetooth reported it (nil if unknown).
@property (nonatomic, readonly, nullable) NSError *lastDisconnectError;

/// Write an output report (haptics / rumble, IDs 0x80-0x89). Report ID first.
- (BOOL)sendOutputReport:(NSData *)report;

/// Write a feature report (report ID first; the ID byte is not sent over BLE).
- (void)sendFeatureReport:(NSData *)report;

/// Write a feature request, then read the controller's reply.
/// The completion runs on the Bluetooth queue; reply is nil on failure.
- (void)queryFeatureReport:(NSData *)request completion:(void (^)(NSData *_Nullable reply))completion;

@end

@interface ILNTritonBLE : NSObject

- (instancetype)initWithDelegate:(id<ILNTritonBLEDelegate>)delegate;

/// @param restoreIdentifier For apps with the bluetooth-central background
///        mode: lets iOS relaunch the app and hand back its controllers if
///        it was terminated in the background. nil disables restoration.
- (instancetype)initWithDelegate:(id<ILNTritonBLEDelegate>)delegate restoreIdentifier:(nullable NSString *)restoreIdentifier;
- (instancetype)init NS_UNAVAILABLE;

/// Controllers seen before (ILNTritonDevice.identifier). Set before -start:
/// each gets a pending connection, which iOS completes whenever the
/// controller is switched on, waking a background app.
@property (nonatomic, copy) NSArray<NSUUID *> *rememberedIdentifiers;

/// Connect to already-paired controllers and scan briefly for new ones.
/// A controller in Bluetooth pairing mode shows the system pairing prompt.
- (void)start;

/// Disconnect from every controller this object connected to.
- (void)stop;

/// Look for controllers in pairing mode for the given number of seconds.
/// Also does what -refresh does.
- (void)scanForNewControllers:(NSTimeInterval)seconds;

/// Pick up controllers that iOS connected while the app wasn't running, and
/// restart any that are connected but not streaming. For when the app comes
/// back to the foreground.
- (void)refresh;

@property (nonatomic, readonly) NSArray<ILNTritonDevice *> *readyDevices;

@end

NS_ASSUME_NONNULL_END
