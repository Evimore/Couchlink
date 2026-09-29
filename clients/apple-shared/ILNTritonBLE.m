//
//  ILNTritonBLE.m
//  InputLine
//
//  GATT layout and pairing behaviour follow SDL's src/hidapi/ios/hid.m
//  (Copyright Valve Corporation, zlib license).
//

#import "ILNTritonBLE.h"

#import <CoreBluetooth/CoreBluetooth.h>

#include <string.h>

static NSString *const kValveService = @"100F6C32-1735-4313-B402-38567131E5F3";
static NSString *const kReportCharacteristic = @"100F6C34-1735-4313-B402-38567131E5F3";
static NSString *const kInput45Characteristic = @"100F6C7A-1735-4313-B402-38567131E5F3";
static NSString *const kInput47Characteristic = @"100F6C7C-1735-4313-B402-38567131E5F3";
static NSString *const kValveUUIDSuffix = @"-1735-4313-B402-38567131E5F3";
static NSString *const kDeviceInformationService = @"180A";

static const size_t kStateReportPayload = 45;
static const NSTimeInterval kNotifyRetryInterval = 1.0;
// How often to look for paired controllers that iOS has reconnected, so a
// controller switched on mid-stream is picked up.
static const NSTimeInterval kKnownControllerPollInterval = 2.0;

#pragma mark - ILNTritonDevice

@interface ILNTritonDevice () <CBPeripheralDelegate>

@property (nonatomic, strong) CBPeripheral *peripheral;
@property (nonatomic, strong) dispatch_queue_t queue;
@property (nonatomic, weak) id<ILNTritonBLEDelegate> delegate;
@property (nonatomic, strong, nullable) CBCharacteristic *inputCharacteristic;
@property (nonatomic, strong, nullable) CBCharacteristic *reportCharacteristic;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, CBCharacteristic *> *outputCharacteristics;
@property (nonatomic, assign) BOOL ready;
@property (nonatomic, assign) uint8_t inputReportId;
@property (nonatomic, strong, nullable) dispatch_source_t notifyRetryTimer;
@property (nonatomic, copy, nullable) void (^pendingFeatureRead)(NSData *_Nullable);
@property (nonatomic, strong, nullable) NSError *lastDisconnectError;

@end

@implementation ILNTritonDevice

- (instancetype)initWithPeripheral:(CBPeripheral *)peripheral queue:(dispatch_queue_t)queue delegate:(id<ILNTritonBLEDelegate>)delegate
{
    if ((self = [super init])) {
        _peripheral = peripheral;
        _queue = queue;
        _delegate = delegate;
        _outputCharacteristics = [NSMutableDictionary dictionary];
        peripheral.delegate = self;
    }
    return self;
}

- (NSUUID *)identifier
{
    return self.peripheral.identifier;
}

- (NSString *)name
{
    return self.peripheral.name ?: @"Steam Controller";
}

- (void)didConnect
{
    [self.peripheral discoverServices:@[[CBUUID UUIDWithString:kValveService]]];
}

- (void)didDisconnect
{
    [self cancelNotifyRetry];
    BOOL wasReady = self.ready;
    self.ready = NO;
    self.inputCharacteristic = nil;
    self.reportCharacteristic = nil;
    [self.outputCharacteristics removeAllObjects];
    void (^pending)(NSData *) = self.pendingFeatureRead;
    self.pendingFeatureRead = nil;
    if (pending) {
        pending(nil);
    }
    if (wasReady) {
        [self.delegate tritonDidDisconnect:self];
    }
}

#pragma mark Writes

- (BOOL)sendOutputReport:(NSData *)report
{
    if (report.length < 2) {
        return NO;
    }
    const uint8_t reportId = ((const uint8_t *)report.bytes)[0];
    NSData *payload = [report subdataWithRange:NSMakeRange(1, report.length - 1)];
    dispatch_async(self.queue, ^{
        CBCharacteristic *characteristic = self.outputCharacteristics[@(reportId)];
        if (characteristic == nil || self.peripheral.state != CBPeripheralStateConnected) {
            return;
        }
        // Haptics are latency-sensitive: skip the acknowledgement when the
        // characteristic allows it.
        CBCharacteristicWriteType type = (characteristic.properties & CBCharacteristicPropertyWriteWithoutResponse)
            ? CBCharacteristicWriteWithoutResponse
            : CBCharacteristicWriteWithResponse;
        [self.peripheral writeValue:payload forCharacteristic:characteristic type:type];
    });
    return YES;
}

- (void)sendFeatureReport:(NSData *)report
{
    if (report.length < 2) {
        return;
    }
    NSData *payload = [report subdataWithRange:NSMakeRange(1, MIN(report.length - 1, (NSUInteger)64))];
    dispatch_async(self.queue, ^{
        if (self.reportCharacteristic == nil || self.peripheral.state != CBPeripheralStateConnected) {
            return;
        }
        [self.peripheral writeValue:payload forCharacteristic:self.reportCharacteristic type:CBCharacteristicWriteWithResponse];
    });
}

- (void)queryFeatureReport:(NSData *)request completion:(void (^)(NSData *_Nullable))completion
{
    dispatch_async(self.queue, ^{
        if (self.reportCharacteristic == nil || self.pendingFeatureRead != nil || request.length < 2) {
            completion(nil);
            return;
        }
        self.pendingFeatureRead = completion;
        NSData *payload = [request subdataWithRange:NSMakeRange(1, MIN(request.length - 1, (NSUInteger)64))];
        [self.peripheral writeValue:payload forCharacteristic:self.reportCharacteristic type:CBCharacteristicWriteWithResponse];
        [self.peripheral readValueForCharacteristic:self.reportCharacteristic];

        // Do not wait forever for a controller that never answers.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(NSEC_PER_SEC / 2)), self.queue, ^{
            void (^pending)(NSData *) = self.pendingFeatureRead;
            if (pending == completion) {
                self.pendingFeatureRead = nil;
                pending(nil);
            }
        });
    });
}

#pragma mark Notification retries

// Enabling notifications silently fails until the user accepts the system
// pairing prompt, and CoreBluetooth never says when that happens. Keep asking
// until the first input report arrives.
- (void)startNotifyRetry
{
    [self cancelNotifyRetry];
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, (uint64_t)(kNotifyRetryInterval * NSEC_PER_SEC), NSEC_PER_SEC / 10);
    __weak ILNTritonDevice *weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        ILNTritonDevice *strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf.ready || strongSelf.inputCharacteristic == nil) {
            [strongSelf cancelNotifyRetry];
            return;
        }
        [strongSelf.peripheral setNotifyValue:YES forCharacteristic:strongSelf.inputCharacteristic];
    });
    self.notifyRetryTimer = timer;
    dispatch_resume(timer);
}

- (void)cancelNotifyRetry
{
    if (self.notifyRetryTimer != nil) {
        dispatch_source_cancel(self.notifyRetryTimer);
        self.notifyRetryTimer = nil;
    }
}

#pragma mark CBPeripheralDelegate

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
    for (CBService *service in peripheral.services) {
        if ([service.UUID isEqual:[CBUUID UUIDWithString:kValveService]]) {
            [peripheral discoverCharacteristics:nil forService:service];
        }
    }
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
    if (![service.UUID isEqual:[CBUUID UUIDWithString:kValveService]]) {
        return;
    }

    CBCharacteristic *input45 = nil;
    CBCharacteristic *input47 = nil;
    for (CBCharacteristic *characteristic in service.characteristics) {
        NSString *uuid = characteristic.UUID.UUIDString.uppercaseString;
        if ([uuid isEqualToString:kInput45Characteristic]) {
            input45 = characteristic;
        } else if ([uuid isEqualToString:kInput47Characteristic]) {
            input47 = characteristic;
        } else if ([uuid isEqualToString:kReportCharacteristic]) {
            self.reportCharacteristic = characteristic;
        } else if ([uuid hasPrefix:@"100F6C"] && [uuid hasSuffix:kValveUUIDSuffix] && uuid.length >= 8) {
            // Output report N lives at 100F6C(N + 0x35).
            unsigned int value = 0;
            NSScanner *scanner = [NSScanner scannerWithString:[uuid substringWithRange:NSMakeRange(6, 2)]];
            if ([scanner scanHexInt:&value] && value > 0x35 && value - 0x35 >= 0x80) {
                self.outputCharacteristics[@(value - 0x35)] = characteristic;
            }
        }
    }

    // Prefer the newer report: it carries a trackpad timestamp and a finer IMU clock.
    if (input47 != nil) {
        self.inputCharacteristic = input47;
        self.inputReportId = 0x47;
    } else if (input45 != nil) {
        self.inputCharacteristic = input45;
        self.inputReportId = 0x45;
    }

    if (self.inputCharacteristic != nil) {
        [peripheral setNotifyValue:YES forCharacteristic:self.inputCharacteristic];
        [self startNotifyRetry];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    if (characteristic == self.inputCharacteristic) {
        NSData *value = characteristic.value;
        if (error != nil || value.length != kStateReportPayload) {
            return;
        }
        if (!self.ready) {
            self.ready = YES;
            [self cancelNotifyRetry];
            [self.delegate tritonDidBecomeReady:self];
        }
        uint8_t report[1 + kStateReportPayload];
        report[0] = self.inputReportId;
        memcpy(report + 1, value.bytes, kStateReportPayload);
        [self.delegate triton:self didReceiveReport:report length:sizeof(report)];
    } else if (characteristic == self.reportCharacteristic) {
        void (^pending)(NSData *) = self.pendingFeatureRead;
        self.pendingFeatureRead = nil;
        if (pending == nil) {
            return;
        }
        if (error != nil || characteristic.value.length == 0) {
            pending(nil);
            return;
        }
        // Match the USB layout: report ID first. Some firmware includes it
        // in the characteristic value already (command IDs are all >= 0x80).
        NSData *value = characteristic.value;
        if (((const uint8_t *)value.bytes)[0] == 0x01) {
            pending(value);
            return;
        }
        const uint8_t reportId = 0x01;
        NSMutableData *reply = [NSMutableData dataWithBytes:&reportId length:1];
        [reply appendData:value];
        pending(reply);
    }
}

@end

#pragma mark - ILNTritonBLE

@interface ILNTritonBLE () <CBCentralManagerDelegate>

@property (nonatomic, weak) id<ILNTritonBLEDelegate> delegate;
@property (nonatomic, strong) dispatch_queue_t queue;
@property (nonatomic, strong, nullable) CBCentralManager *central;
@property (nonatomic, strong) NSMutableDictionary<NSUUID *, ILNTritonDevice *> *devices;
@property (nonatomic, assign) BOOL running;
@property (nonatomic, assign) NSUInteger scanGeneration;
@property (nonatomic, strong, nullable) dispatch_source_t pollTimer;
@property (nonatomic, copy, nullable) NSString *restoreIdentifier;

@end

@implementation ILNTritonBLE

- (instancetype)initWithDelegate:(id<ILNTritonBLEDelegate>)delegate
{
    return [self initWithDelegate:delegate restoreIdentifier:nil];
}

- (instancetype)initWithDelegate:(id<ILNTritonBLEDelegate>)delegate restoreIdentifier:(NSString *)restoreIdentifier
{
    if ((self = [super init])) {
        _restoreIdentifier = [restoreIdentifier copy];
        _delegate = delegate;
        // Reports must be drained promptly or iOS may quietly stop delivering
        // them, so use a dedicated high-priority serial queue.
        _queue = dispatch_queue_create("com.evimore.inputline.ble", DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(_queue, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
        _devices = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void)start
{
    dispatch_async(self.queue, ^{
        if (self.running) {
            return;
        }
        self.running = YES;
        if (self.central == nil) {
            // Creating the manager triggers centralManagerDidUpdateState:.
            NSDictionary *options = self.restoreIdentifier != nil ? @{CBCentralManagerOptionRestoreIdentifierKey: self.restoreIdentifier} : nil;
            self.central = [[CBCentralManager alloc] initWithDelegate:self queue:self.queue options:options];
        } else if (self.central.state == CBManagerStatePoweredOn) {
            [self connectRememberedControllers];
            [self connectKnownControllers];
            [self startPolling];
            [self scanLocked:20];
        }
    });
}

- (void)stop
{
    dispatch_sync(self.queue, ^{
        self.running = NO;
        [self stopPolling];
        [self.central stopScan];
        for (ILNTritonDevice *device in self.devices.allValues) {
            [self.central cancelPeripheralConnection:device.peripheral];
            [device didDisconnect];
        }
        [self.devices removeAllObjects];
    });
}

- (void)scanForNewControllers:(NSTimeInterval)seconds
{
    dispatch_async(self.queue, ^{
        [self scanLocked:seconds];
    });
}

- (NSArray<ILNTritonDevice *> *)readyDevices
{
    __block NSArray<ILNTritonDevice *> *result = nil;
    dispatch_sync(self.queue, ^{
        NSMutableArray *ready = [NSMutableArray array];
        for (ILNTritonDevice *device in self.devices.allValues) {
            if (device.ready) {
                [ready addObject:device];
            }
        }
        result = ready;
    });
    return result;
}

#pragma mark Internals (Bluetooth queue)

- (void)scanLocked:(NSTimeInterval)seconds
{
    if (!self.running || self.central.state != CBManagerStatePoweredOn) {
        return;
    }
    // The Valve service UUID does not fit in the base advertising packet, so
    // scan everything and filter by name, as SDL does.
    [self.central scanForPeripheralsWithServices:nil options:nil];
    const NSUInteger generation = ++self.scanGeneration;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), self.queue, ^{
        if (generation == self.scanGeneration) {
            [self.central stopScan];
        }
    });
}

// A paired controller that wakes up reconnects to iOS on its own; iOS does
// not tell apps, so ask periodically. This is a local query, not a scan.
- (void)startPolling
{
    if (self.pollTimer != nil) {
        return;
    }
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
    const uint64_t interval = (uint64_t)(kKnownControllerPollInterval * NSEC_PER_SEC);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)interval), interval, NSEC_PER_SEC / 4);
    __weak ILNTritonBLE *weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        ILNTritonBLE *strongSelf = weakSelf;
        if (strongSelf.running && strongSelf.central.state == CBManagerStatePoweredOn) {
            [strongSelf connectKnownControllers];
        }
    });
    self.pollTimer = timer;
    dispatch_resume(timer);
}

- (void)stopPolling
{
    if (self.pollTimer != nil) {
        dispatch_source_cancel(self.pollTimer);
        self.pollTimer = nil;
    }
}

- (void)connectRememberedControllers
{
    if (self.rememberedIdentifiers.count == 0) {
        return;
    }
    for (CBPeripheral *peripheral in [self.central retrievePeripheralsWithIdentifiers:self.rememberedIdentifiers]) {
        [self connectPeripheral:peripheral];
    }
}

- (void)connectKnownControllers
{
    NSArray<CBUUID *> *services = @[[CBUUID UUIDWithString:kValveService], [CBUUID UUIDWithString:kDeviceInformationService]];
    for (CBPeripheral *peripheral in [self.central retrieveConnectedPeripheralsWithServices:services]) {
        if ([peripheral.name hasPrefix:@"Steam"]) {
            [self connectPeripheral:peripheral];
        }
    }
}

- (void)connectPeripheral:(CBPeripheral *)peripheral
{
    if (self.devices[peripheral.identifier] != nil) {
        return;
    }
    ILNTritonDevice *device = [[ILNTritonDevice alloc] initWithPeripheral:peripheral queue:self.queue delegate:self.delegate];
    self.devices[peripheral.identifier] = device;
    [self.central connectPeripheral:peripheral options:nil];
}

#pragma mark CBCentralManagerDelegate

// iOS relaunched the app in the background and hands back the controllers
// it was connected to. Called before centralManagerDidUpdateState:.
- (void)centralManager:(CBCentralManager *)central willRestoreState:(NSDictionary<NSString *, id> *)state
{
    for (CBPeripheral *peripheral in state[CBCentralManagerRestoredStatePeripheralsKey]) {
        if (![peripheral.name hasPrefix:@"Steam"] || self.devices[peripheral.identifier] != nil) {
            continue;
        }
        ILNTritonDevice *device = [[ILNTritonDevice alloc] initWithPeripheral:peripheral queue:self.queue delegate:self.delegate];
        self.devices[peripheral.identifier] = device;
        if (peripheral.state == CBPeripheralStateConnected) {
            [device didConnect];
        } else {
            [central connectPeripheral:peripheral options:nil];
        }
    }
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    if (central.state == CBManagerStatePoweredOn && self.running) {
        [self connectRememberedControllers];
        [self connectKnownControllers];
        [self startPolling];
        [self scanLocked:20];
    } else {
        [self stopPolling];
        if (central.state == CBManagerStateUnauthorized && self.running) {
            id<ILNTritonBLEDelegate> delegate = self.delegate;
            if ([delegate respondsToSelector:@selector(tritonBluetoothUnauthorized)]) {
                [delegate tritonBluetoothUnauthorized];
            }
        }
    }
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
    NSString *name = advertisementData[CBAdvertisementDataLocalNameKey] ?: peripheral.name;
    if (self.running && [name hasPrefix:@"Steam"]) {
        [self connectPeripheral:peripheral];
    }
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
    [self.devices[peripheral.identifier] didConnect];
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    [self.devices removeObjectForKey:peripheral.identifier];
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    ILNTritonDevice *device = self.devices[peripheral.identifier];
    device.lastDisconnectError = error;
    [device didDisconnect];
    if (self.running && device != nil) {
        // A pending connect never times out: the controller reconnects as soon
        // as it wakes up or comes back into range.
        [central connectPeripheral:peripheral options:nil];
    } else {
        [self.devices removeObjectForKey:peripheral.identifier];
    }
}

@end
