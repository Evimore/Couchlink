//
//  ILNViewController.m
//  InputLine
//
//  One screen: the PC, the controller, and how smoothly Bluetooth reports
//  arrive (in the foreground and while another app is in front).
//

#import "ILNViewController.h"
#import "ILNBridge.h"
#import "ILNDiscovery.h"

@interface ILNViewController () <ILNBridgeDelegate, UITextFieldDelegate>
@end

@implementation ILNViewController {
    UITextField *_addressField;
    UIStackView *_discoveredStack;
    NSString *_discoveredShown;  // what _discoveredStack shows, to rebuild only on change
    NSArray<NSString *> *_discoveredAddresses;  // by button tag
    UILabel *_linkLabel;
    UILabel *_controllersLabel;
    UILabel *_timingNowLabel;
    UILabel *_timingForegroundLabel;
    UILabel *_timingBackgroundLabel;
    UILabel *_eventsLabel;
    UIButton *_shareButton;
    UIButton *_pauseButton;
    UILabel *_pauseNote;
    UIAlertController *_codeAlert;
    NSTimer *_refreshTimer;
}

#pragma mark - Layout helpers

- (UILabel *)labelWithStyle:(UIFontTextStyle)style color:(UIColor *)color
{
    UILabel *label = [[UILabel alloc] init];
    label.font = [UIFont preferredFontForTextStyle:style];
    label.adjustsFontForContentSizeCategory = YES;
    label.textColor = color;
    label.numberOfLines = 0;
    return label;
}

- (UILabel *)heading:(NSString *)text
{
    UILabel *label = [self labelWithStyle:UIFontTextStyleHeadline color:[UIColor labelColor]];
    label.text = text;
    return label;
}

- (UILabel *)note:(NSString *)text
{
    UILabel *label = [self labelWithStyle:UIFontTextStyleFootnote color:[UIColor secondaryLabelColor]];
    label.text = text;
    return label;
}

- (UIButton *)buttonWithTitle:(NSString *)title action:(SEL)action
{
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:title forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

#pragma mark - View

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemGroupedBackgroundColor];

    UIScrollView *scroll = [[UIScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [self.view addSubview:scroll];

    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];

    UILayoutGuide *readable = self.view.readableContentGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],
        [stack.leadingAnchor constraintEqualToAnchor:readable.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:readable.trailingAnchor],
    ]];

    UILabel *title = [self labelWithStyle:UIFontTextStyleLargeTitle color:[UIColor labelColor]];
    title.text = @"InputLine";
    [stack addArrangedSubview:title];
    [stack addArrangedSubview:[self note:@"Your Steam Controller, with full Steam Input on your gaming PC, next to any streaming app. "
                                          "Set it up here, then switch to your streaming app: InputLine keeps working in the background.\n\nDon't swipe InputLine away in the app switcher: iOS then disconnects the controller and doesn't restart InputLine until you open it again."]];
    [stack setCustomSpacing:24 afterView:stack.arrangedSubviews.lastObject];

    // PC
    [stack addArrangedSubview:[self heading:@"Gaming PC"]];
    _addressField = [[UITextField alloc] init];
    _addressField.borderStyle = UITextBorderStyleRoundedRect;
    _addressField.placeholder = @"PC address, e.g. 192.168.1.20";
    _addressField.keyboardType = UIKeyboardTypeURL;
    _addressField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    _addressField.autocorrectionType = UITextAutocorrectionTypeNo;
    _addressField.returnKeyType = UIReturnKeyGo;
    _addressField.delegate = self;
    _addressField.text = [ILNBridge shared].status.pcAddress;
    [stack addArrangedSubview:_addressField];
    [stack addArrangedSubview:[self buttonWithTitle:@"Connect" action:@selector(connect)]];
    _discoveredStack = [[UIStackView alloc] init];
    _discoveredStack.axis = UILayoutConstraintAxisVertical;
    _discoveredStack.alignment = UIStackViewAlignmentLeading;
    _discoveredStack.spacing = 4;
    _discoveredStack.hidden = YES;
    [stack addArrangedSubview:_discoveredStack];
    _linkLabel = [self labelWithStyle:UIFontTextStyleBody color:[UIColor labelColor]];
    [stack addArrangedSubview:_linkLabel];
    [stack addArrangedSubview:[self note:@"The PC needs InputLine installed (see the Releases page). At home, InputLine finds it on the network by itself. Away from home, enter its address on your VPN (for example Tailscale)."]];
    [stack setCustomSpacing:24 afterView:stack.arrangedSubviews.lastObject];

    // Controller
    [stack addArrangedSubview:[self heading:@"Controller"]];
    _controllersLabel = [self labelWithStyle:UIFontTextStyleBody color:[UIColor labelColor]];
    [stack addArrangedSubview:_controllersLabel];
    _pauseButton = [self buttonWithTitle:@"Disconnect from PC" action:@selector(togglePaused)];
    _pauseButton.hidden = YES;
    [stack addArrangedSubview:_pauseButton];
    _pauseNote = [self note:@"Disconnect to use the controller with this device (its pointer works again). "
                             "It goes back to the PC when you tap Connect to PC, or when you switch the controller off and on."];
    _pauseNote.hidden = YES;
    [stack addArrangedSubview:_pauseNote];
    [stack addArrangedSubview:[self buttonWithTitle:@"Pair a new controller" action:@selector(pairController)]];
    [stack addArrangedSubview:[self note:@"Put the controller in Bluetooth pairing mode first, then tap the button and accept the pairing request. "
                                          "Paired controllers reconnect by themselves when you switch them on."]];
    [stack setCustomSpacing:24 afterView:stack.arrangedSubviews.lastObject];

    // Timing
    [stack addArrangedSubview:[self heading:@"Bluetooth timing on this device"]];
    [stack addArrangedSubview:[self note:@"How evenly controller reports arrive. Over Bluetooth on iPad and iPhone, about 67 per second (one every 15 ms) is normal. "
                                          "Play a while with your streaming app in front, then come back to compare."]];
    _timingNowLabel = [self labelWithStyle:UIFontTextStyleFootnote color:[UIColor labelColor]];
    _timingForegroundLabel = [self labelWithStyle:UIFontTextStyleFootnote color:[UIColor labelColor]];
    _timingBackgroundLabel = [self labelWithStyle:UIFontTextStyleFootnote color:[UIColor labelColor]];
    for (UILabel *label in @[_timingNowLabel, _timingForegroundLabel, _timingBackgroundLabel]) {
        label.font = [UIFont monospacedSystemFontOfSize:[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote].pointSize weight:UIFontWeightRegular];
        [stack addArrangedSubview:label];
    }
    [stack setCustomSpacing:24 afterView:stack.arrangedSubviews.lastObject];

    // Events
    [stack addArrangedSubview:[self heading:@"Recent events"]];
    _eventsLabel = [self labelWithStyle:UIFontTextStyleCaption1 color:[UIColor secondaryLabelColor]];
    _eventsLabel.font = [UIFont monospacedSystemFontOfSize:[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1].pointSize weight:UIFontWeightRegular];
    [stack addArrangedSubview:_eventsLabel];

    _shareButton = [self buttonWithTitle:@"Share report" action:@selector(shareReport)];
    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[
        _shareButton,
        [self buttonWithTitle:@"Copy" action:@selector(copyReport)],
        [self buttonWithTitle:@"Reset timing" action:@selector(resetTiming)],
    ]];
    buttons.spacing = 20;
    buttons.alignment = UIStackViewAlignmentLeading;
    // A narrow iPhone with large text can't fit three buttons side by side.
    if (UIContentSizeCategoryIsAccessibilityCategory(self.traitCollection.preferredContentSizeCategory)) {
        buttons.axis = UILayoutConstraintAxisVertical;
        buttons.spacing = 8;
    }
    [buttons addArrangedSubview:[[UIView alloc] init]];
    [stack addArrangedSubview:buttons];
    [stack addArrangedSubview:[self note:@"When something goes wrong, tap Share report and send it along with the PC's log."]];

    [ILNBridge shared].delegate = self;
    [self refresh];
}

- (void)viewWillAppear:(BOOL)animated
{
    [super viewWillAppear:animated];
    _refreshTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(refresh) userInfo:nil repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated
{
    [super viewWillDisappear:animated];
    [_refreshTimer invalidate];
    _refreshTimer = nil;
}

- (void)refresh
{
    ILNStatus *status = [ILNBridge shared].status;
    _linkLabel.text = status.linkText;
    [_pauseButton setTitle:status.paused ? @"Connect to PC" : @"Disconnect from PC" forState:UIControlStateNormal];
    _pauseButton.hidden = status.controllers.count == 0;
    _pauseNote.hidden = _pauseButton.hidden;
    [self showDiscoveredPCs:status];
    _controllersLabel.text = status.controllers.count > 0 ? [status.controllers componentsJoinedByString:@"\n"]
                                                          : @"No controller connected. Switch it on, or pair a new one.";
    _timingNowLabel.text = [NSString stringWithFormat:@"Now:        %@", status.timingNow.length > 0 ? status.timingNow : @"-"];
    _timingForegroundLabel.text = [NSString stringWithFormat:@"Foreground: %@", status.timingForeground.length > 0 ? status.timingForeground : @"-"];
    _timingBackgroundLabel.text = [NSString stringWithFormat:@"Background: %@", status.timingBackground.length > 0 ? status.timingBackground : @"-"];
    NSArray<NSString *> *recent = status.events.count > 12 ? [status.events subarrayWithRange:NSMakeRange(status.events.count - 12, 12)] : status.events;
    _eventsLabel.text = recent.count > 0 ? [recent componentsJoinedByString:@"\n"] : @"-";
}

- (void)showDiscoveredPCs:(ILNStatus *)status
{
    NSMutableArray<ILNDiscoveredPC *> *others = [NSMutableArray array];
    NSMutableString *key = [NSMutableString string];
    for (ILNDiscoveredPC *pc in status.discoveredPCs) {
        if (![pc.address isEqualToString:status.pcAddress]) {
            [others addObject:pc];
            [key appendFormat:@"%@=%@;", pc.name, pc.address];
        }
    }
    // With nothing saved yet, suggest the only PC around.
    if (status.pcAddress.length == 0 && status.discoveredPCs.count == 1 && _addressField.text.length == 0 && !_addressField.isEditing) {
        _addressField.text = status.discoveredPCs.firstObject.address;
    }
    if ([key isEqualToString:_discoveredShown ?: @""]) {
        return;
    }
    _discoveredShown = key;
    for (UIView *view in _discoveredStack.arrangedSubviews) {
        [view removeFromSuperview];
    }
    if (others.count > 0) {
        [_discoveredStack addArrangedSubview:[self note:@"Found on this network:"]];
    }
    NSMutableArray<NSString *> *addresses = [NSMutableArray array];
    for (ILNDiscoveredPC *pc in others) {
        UIButton *button = [self buttonWithTitle:[NSString stringWithFormat:@"Use %@ (%@)", pc.name, pc.address] action:@selector(useDiscoveredPC:)];
        button.titleLabel.numberOfLines = 0;
        button.tag = (NSInteger)addresses.count;
        [addresses addObject:pc.address];
        [_discoveredStack addArrangedSubview:button];
    }
    _discoveredAddresses = addresses;
    _discoveredStack.hidden = others.count == 0;
}

#pragma mark - Actions

- (void)togglePaused
{
    [[ILNBridge shared] setPaused:![ILNBridge shared].status.paused];
    [self refresh];
}

- (void)useDiscoveredPC:(UIButton *)sender
{
    if (sender.tag < 0 || (NSUInteger)sender.tag >= _discoveredAddresses.count) {
        return;
    }
    _addressField.text = _discoveredAddresses[(NSUInteger)sender.tag];
    [self connect];
}

- (void)connect
{
    [_addressField resignFirstResponder];
    [[ILNBridge shared] connectToPC:_addressField.text ?: @""];
    [self refresh];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField
{
    [self connect];
    return YES;
}

- (void)pairController
{
    [[ILNBridge shared] scanForNewControllers];
    [self bridge:[ILNBridge shared] showMessage:@"Looking for controllers in pairing mode for 30 seconds."];
}

- (void)shareReport
{
    UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:@[[[ILNBridge shared] report]] applicationActivities:nil];
    // On iPad the share sheet is a popover and needs an anchor; iPhone ignores it.
    share.popoverPresentationController.sourceView = _shareButton;
    share.popoverPresentationController.sourceRect = _shareButton.bounds;
    [self presentViewController:share animated:YES completion:nil];
}

- (void)copyReport
{
    [UIPasteboard generalPasteboard].string = [[ILNBridge shared] report];
    [self bridge:[ILNBridge shared] showMessage:@"Copied the report."];
}

- (void)resetTiming
{
    [[ILNBridge shared] resetTiming];
}

#pragma mark - ILNBridgeDelegate

- (void)bridge:(ILNBridge *)bridge needsPairingCodeWithMessage:(NSString *)message
{
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Pair with your PC" message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = @"6-digit code";
        field.keyboardType = UIKeyboardTypeNumberPad;
    }];
    __weak UIAlertController *weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        [[ILNBridge shared] cancelPairing];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Pair" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        [[ILNBridge shared] submitPairingCode:weakAlert.textFields.firstObject.text ?: @""];
    }]];

    void (^present)(void) = ^{
        self->_codeAlert = alert;
        [self presentViewController:alert animated:YES completion:nil];
    };
    if (self.presentedViewController != nil) {
        [self dismissViewControllerAnimated:NO completion:present];
    } else {
        present();
    }
}

- (void)bridge:(ILNBridge *)bridge showMessage:(NSString *)message
{
    if (self.presentedViewController != nil) {
        return;  // don't cover a pairing prompt
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:nil message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end
