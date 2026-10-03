//
//  ILNViewController.m
//  InputLine
//
//  One screen of cards: the PC, the controller, and diagnostics (how
//  smoothly Bluetooth reports arrive, recent events, the shareable report).
//  Each card starts with a status row that stays in place while the rest of
//  the card changes around it.
//

#import "ILNViewController.h"
#import "ILNBridge.h"
#import "ILNDiscovery.h"

NSString *const ILNScreenshotKey = @"ILNScreenshot";

static NSString *const kDiagnosticsExpandedKey = @"ILNDiagnosticsExpanded";

#pragma mark - Colours

static UIColor *ILNDynamicColor(UIColor *light, UIColor *dark)
{
    return [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
        return traits.userInterfaceStyle == UIUserInterfaceStyleDark ? dark : light;
    }];
}

// A shade darker than the system colours in light mode, so status text stays readable.
static UIColor *ILNGreen(void)
{
    return ILNDynamicColor([UIColor colorWithRed:0.10 green:0.55 blue:0.25 alpha:1], [UIColor systemGreenColor]);
}

static UIColor *ILNRed(void)
{
    return ILNDynamicColor([UIColor colorWithRed:0.82 green:0.16 blue:0.15 alpha:1], [UIColor systemRedColor]);
}

static UIColor *ILNOrange(void)
{
    return ILNDynamicColor([UIColor colorWithRed:0.80 green:0.42 blue:0.00 alpha:1], [UIColor systemOrangeColor]);
}

static UIColor *ILNGray(void)
{
    return [UIColor systemGrayColor];
}

#pragma mark - Battery

/// A battery like the one in the status bar: filled to the level, green
/// with a lightning bolt while plugged in, red when low.
@interface ILNBatteryView : UIView
@property (nonatomic) NSInteger level;  // 0-100
@property (nonatomic) BOOL charging;
@end

@implementation ILNBatteryView

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self != nil) {
        self.opaque = NO;
        self.backgroundColor = [UIColor clearColor];
        self.contentMode = UIViewContentModeRedraw;
    }
    return self;
}

- (CGSize)intrinsicContentSize
{
    return CGSizeMake(27, 13);
}

- (void)setLevel:(NSInteger)level
{
    _level = MAX((NSInteger)0, MIN(level, (NSInteger)100));
    [self setNeedsDisplay];
}

- (void)setCharging:(BOOL)charging
{
    _charging = charging;
    [self setNeedsDisplay];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previous
{
    [super traitCollectionDidChange:previous];
    [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect
{
    const CGRect bounds = self.bounds;
    const CGFloat nubWidth = 2;
    const CGRect body = CGRectMake(0.5, 0.5, bounds.size.width - nubWidth - 1.5, bounds.size.height - 1);

    UIBezierPath *outline = [UIBezierPath bezierPathWithRoundedRect:body cornerRadius:3.5];
    outline.lineWidth = 1;
    [[[UIColor labelColor] colorWithAlphaComponent:0.35] setStroke];
    [outline stroke];

    UIBezierPath *nub = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(CGRectGetMaxX(body) + 1, CGRectGetMidY(body) - 2.25, nubWidth - 0.5, 4.5)
                                              byRoundingCorners:UIRectCornerTopRight | UIRectCornerBottomRight
                                                    cornerRadii:CGSizeMake(1, 1)];
    [[[UIColor labelColor] colorWithAlphaComponent:0.35] setFill];
    [nub fill];

    const CGRect inside = CGRectInset(body, 2, 2);
    UIColor *fill = self.charging ? ILNGreen() : self.level <= 20 ? ILNRed() : [UIColor labelColor];
    if (self.level > 0) {
        const CGFloat width = MAX(2, inside.size.width * (CGFloat)self.level / 100);
        [fill setFill];
        [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(inside.origin.x, inside.origin.y, width, inside.size.height) cornerRadius:1.5] fill];
    }

    if (self.charging) {
        // The bolt, with a thin outline in the card's colour so it reads over the fill.
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:10 weight:UIImageSymbolWeightBlack];
        UIImage *bolt = [UIImage systemImageNamed:@"bolt.fill" withConfiguration:config];
        const CGSize size = bolt.size;
        const CGRect place = CGRectMake(CGRectGetMidX(body) - size.width / 2, CGRectGetMidY(body) - size.height / 2, size.width, size.height);
        UIImage *halo = [bolt imageWithTintColor:[UIColor secondarySystemGroupedBackgroundColor] renderingMode:UIImageRenderingModeAlwaysOriginal];
        for (CGFloat dx = -1; dx <= 1; dx += 1) {
            for (CGFloat dy = -1; dy <= 1; dy += 1) {
                [halo drawInRect:CGRectOffset(place, dx, dy)];
            }
        }
        [[bolt imageWithTintColor:[UIColor labelColor] renderingMode:UIImageRenderingModeAlwaysOriginal] drawInRect:place];
    }
}

@end

#pragma mark - Status row

/// A round symbol, a title and a coloured status line: the top of a card.
@interface ILNStatusRow : UIView
- (void)setSymbol:(NSString *)symbol title:(NSString *)title status:(NSString *)status color:(UIColor *)color busy:(BOOL)busy;
/// A battery on the right; level -1 hides it.
- (void)setBatteryLevel:(NSInteger)level charging:(BOOL)charging;
@end

@implementation ILNStatusRow {
    UIView *_badge;
    UIImageView *_icon;
    UILabel *_title;
    UIView *_dot;
    UILabel *_status;
    UIActivityIndicatorView *_spinner;
    UIStackView *_battery;
    ILNBatteryView *_batteryIcon;
    UILabel *_batteryText;
    NSString *_accessibilityBase;
}

- (instancetype)init
{
    self = [super initWithFrame:CGRectZero];
    if (self == nil) {
        return nil;
    }
    _badge = [[UIView alloc] init];
    _badge.layer.cornerRadius = 22;
    _badge.translatesAutoresizingMaskIntoConstraints = NO;
    _icon = [[UIImageView alloc] init];
    _icon.contentMode = UIViewContentModeScaleAspectFit;
    _icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:19 weight:UIImageSymbolWeightSemibold];
    _icon.translatesAutoresizingMaskIntoConstraints = NO;
    [_badge addSubview:_icon];

    _title = [[UILabel alloc] init];
    _title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _title.adjustsFontForContentSizeCategory = YES;
    _title.numberOfLines = 0;

    _dot = [[UIView alloc] init];
    _dot.layer.cornerRadius = 4;
    _dot.translatesAutoresizingMaskIntoConstraints = NO;
    _status = [[UILabel alloc] init];
    UIFontDescriptor *subheadline = [UIFontDescriptor preferredFontDescriptorWithTextStyle:UIFontTextStyleSubheadline];
    _status.font = [UIFont fontWithDescriptor:[subheadline fontDescriptorByAddingAttributes:@{
                                                  UIFontDescriptorTraitsAttribute: @{UIFontWeightTrait: @(UIFontWeightMedium)},
                                              }]
                                         size:0];
    _status.numberOfLines = 0;
    _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _spinner.hidesWhenStopped = YES;
    _spinner.transform = CGAffineTransformMakeScale(0.75, 0.75);

    UIStackView *statusLine = [[UIStackView alloc] initWithArrangedSubviews:@[_dot, _status, _spinner]];
    statusLine.spacing = 6;
    statusLine.alignment = UIStackViewAlignmentCenter;
    UIStackView *texts = [[UIStackView alloc] initWithArrangedSubviews:@[_title, statusLine]];
    texts.axis = UILayoutConstraintAxisVertical;
    texts.spacing = 2;
    texts.alignment = UIStackViewAlignmentLeading;
    _batteryIcon = [[ILNBatteryView alloc] initWithFrame:CGRectZero];
    _batteryText = [[UILabel alloc] init];
    _batteryText.font = [UIFont monospacedDigitSystemFontOfSize:[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline].pointSize
                                                         weight:UIFontWeightMedium];
    _batteryText.textColor = [UIColor secondaryLabelColor];
    _battery = [[UIStackView alloc] initWithArrangedSubviews:@[_batteryText, _batteryIcon]];
    _battery.spacing = 5;
    _battery.alignment = UIStackViewAlignmentCenter;
    _battery.hidden = YES;
    [_battery setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_battery setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[_badge, texts, _battery]];
    row.spacing = 12;
    row.alignment = UIStackViewAlignmentCenter;
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:row];
    [NSLayoutConstraint activateConstraints:@[
        [_badge.widthAnchor constraintEqualToConstant:44],
        [_badge.heightAnchor constraintEqualToConstant:44],
        [_icon.centerXAnchor constraintEqualToAnchor:_badge.centerXAnchor],
        [_icon.centerYAnchor constraintEqualToAnchor:_badge.centerYAnchor],
        [_dot.widthAnchor constraintEqualToConstant:8],
        [_dot.heightAnchor constraintEqualToConstant:8],
        [row.topAnchor constraintEqualToAnchor:self.topAnchor],
        [row.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [row.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [row.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    ]];
    self.isAccessibilityElement = YES;
    return self;
}

- (void)setSymbol:(NSString *)symbol title:(NSString *)title status:(NSString *)status color:(UIColor *)color busy:(BOOL)busy
{
    _icon.image = [UIImage systemImageNamed:symbol];
    _icon.tintColor = color;
    _badge.backgroundColor = [color colorWithAlphaComponent:0.15];
    _title.text = title;
    _status.text = status;
    _status.textColor = color;
    _dot.backgroundColor = color;
    if (busy) {
        [_spinner startAnimating];
    } else {
        [_spinner stopAnimating];
    }
    _accessibilityBase = [NSString stringWithFormat:@"%@, %@", title, status];
    [self updateAccessibility];
}

- (void)setBatteryLevel:(NSInteger)level charging:(BOOL)charging
{
    _battery.hidden = level < 0;
    if (level >= 0) {
        _batteryIcon.level = level;
        _batteryIcon.charging = charging;
        _batteryText.text = [NSString stringWithFormat:@"%ld%%", (long)level];
    }
    [self updateAccessibility];
}

- (void)updateAccessibility
{
    if (_battery.hidden) {
        self.accessibilityLabel = _accessibilityBase;
        return;
    }
    self.accessibilityLabel = [NSString stringWithFormat:@"%@, battery %@%@", _accessibilityBase, _batteryText.text, _batteryIcon.charging ? @", charging" : @""];
}

@end

#pragma mark - View controller

typedef NS_ENUM(NSInteger, ILNButtonStyle) {
    ILNButtonStylePrimary = 1,  // filled
    ILNButtonStyleDestructive,  // tinted red
    ILNButtonStyleSecondary,    // gray
    ILNButtonStyleLink,         // plain text
};

@interface ILNViewController () <ILNBridgeDelegate, UITextFieldDelegate>
@end

@implementation ILNViewController {
    NSString *_screenshotState;  // nil unless taking screenshots
    NSMutableArray<void (^)(void)> *_layoutChanges;  // views to show or hide, animated together

    // PC
    ILNStatusRow *_pcRow;
    UILabel *_pcDetail;
    UILabel *_updateLabel;
    UITextField *_addressField;
    UIButton *_connectButton;
    UIStackView *_discoveredStack;
    NSString *_discoveredShown;  // what _discoveredStack shows, to rebuild only on change
    NSArray<NSString *> *_discoveredAddresses;  // by button tag
    UILabel *_pcTip;

    // Controller
    UIStackView *_controllerRows;
    UIButton *_pauseButton;
    UILabel *_pauseNote;
    UIButton *_pairButton;
    UILabel *_pairNote;

    // Diagnostics
    UIButton *_shareButton;
    UIButton *_detailsButton;
    UIStackView *_detailsStack;
    UILabel *_timingNowLabel;
    UILabel *_timingForegroundLabel;
    UILabel *_timingBackgroundLabel;
    UILabel *_eventsLabel;

    UIAlertController *_codeAlert;
    NSTimer *_refreshTimer;
}

#pragma mark - Building blocks

- (UILabel *)labelWithStyle:(UIFontTextStyle)style color:(UIColor *)color
{
    UILabel *label = [[UILabel alloc] init];
    label.font = [UIFont preferredFontForTextStyle:style];
    label.adjustsFontForContentSizeCategory = YES;
    label.textColor = color;
    label.numberOfLines = 0;
    return label;
}

- (UILabel *)note:(NSString *)text
{
    UILabel *label = [self labelWithStyle:UIFontTextStyleFootnote color:[UIColor secondaryLabelColor]];
    label.text = text;
    return label;
}

- (UILabel *)heading:(NSString *)text
{
    UILabel *label = [self labelWithStyle:UIFontTextStyleHeadline color:[UIColor labelColor]];
    label.text = text;
    return label;
}

- (UILabel *)monospaced:(UIFontTextStyle)style
{
    UILabel *label = [self labelWithStyle:style color:[UIColor labelColor]];
    label.font = [UIFont monospacedSystemFontOfSize:[UIFont preferredFontForTextStyle:style].pointSize weight:UIFontWeightRegular];
    return label;
}

- (UIButton *)buttonWithAction:(SEL)action
{
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)styleButton:(UIButton *)button title:(NSString *)title style:(ILNButtonStyle)style
{
    if (button.tag == style && [button.configuration.title isEqualToString:title]) {
        return;  // unchanged: don't disturb a press in progress
    }
    button.tag = style;
    UIButtonConfiguration *config = nil;
    switch (style) {
        case ILNButtonStylePrimary:
            config = [UIButtonConfiguration filledButtonConfiguration];
            break;
        case ILNButtonStyleDestructive:
            config = [UIButtonConfiguration tintedButtonConfiguration];
            config.baseForegroundColor = ILNRed();
            config.baseBackgroundColor = [UIColor systemRedColor];
            break;
        case ILNButtonStyleSecondary:
            config = [UIButtonConfiguration grayButtonConfiguration];
            break;
        case ILNButtonStyleLink:
            config = [UIButtonConfiguration plainButtonConfiguration];
            config.contentInsets = NSDirectionalEdgeInsetsMake(4, 0, 4, 0);
            break;
    }
    config.title = title;
    UIFontTextStyle textStyle = style == ILNButtonStyleLink ? UIFontTextStyleBody : UIFontTextStyleHeadline;
    config.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id> *(NSDictionary<NSAttributedStringKey, id> *attributes) {
        NSMutableDictionary<NSAttributedStringKey, id> *changed = [attributes mutableCopy];
        changed[NSFontAttributeName] = [UIFont preferredFontForTextStyle:textStyle];
        return changed;
    };
    if (style != ILNButtonStyleLink) {
        config.buttonSize = UIButtonConfigurationSizeLarge;
        config.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    }
    button.configuration = config;
    button.contentHorizontalAlignment = style == ILNButtonStyleLink ? UIControlContentHorizontalAlignmentLeading : UIControlContentHorizontalAlignmentCenter;
}

/// A section title and a rounded card under it; returns the card's content stack.
- (UIStackView *)addCardTo:(UIStackView *)page title:(NSString *)title
{
    UILabel *header = [self labelWithStyle:UIFontTextStyleFootnote color:[UIColor secondaryLabelColor]];
    header.text = title.uppercaseString;
    header.accessibilityLabel = title;
    header.accessibilityTraits = UIAccessibilityTraitHeader;
    UIStackView *headerRow = [[UIStackView alloc] initWithArrangedSubviews:@[header]];
    headerRow.layoutMarginsRelativeArrangement = YES;
    headerRow.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(0, 16, 0, 16);
    [page addArrangedSubview:headerRow];
    [page setCustomSpacing:8 afterView:headerRow];

    UIView *card = [[UIView alloc] init];
    card.backgroundColor = [UIColor secondarySystemGroupedBackgroundColor];
    card.layer.cornerRadius = 16;
    card.layer.cornerCurve = kCACornerCurveContinuous;
    UIStackView *content = [[UIStackView alloc] init];
    content.axis = UILayoutConstraintAxisVertical;
    content.spacing = 14;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.topAnchor constraintEqualToAnchor:card.topAnchor constant:16],
        [content.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-16],
        [content.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16],
        [content.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16],
    ]];
    [page addArrangedSubview:card];
    [page setCustomSpacing:28 afterView:card];
    return content;
}

/// Show or hide @p view; the changes of one refresh animate together.
- (void)setView:(UIView *)view visible:(BOOL)visible
{
    if (view.hidden != visible) {
        return;  // already so
    }
    [_layoutChanges addObject:^{
        view.hidden = !visible;
        view.alpha = visible ? 1 : 0;
    }];
}

- (NSString *)deviceName
{
    return [UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPad ? @"iPad" : @"iPhone";
}

#pragma mark - View

- (void)viewDidLoad
{
    [super viewDidLoad];
    _screenshotState = [[NSUserDefaults standardUserDefaults] stringForKey:ILNScreenshotKey];
    _layoutChanges = [NSMutableArray array];
    self.view.backgroundColor = [UIColor systemGroupedBackgroundColor];

    UIScrollView *scroll = [[UIScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    scroll.alwaysBounceVertical = YES;
    [self.view addSubview:scroll];

    UIStackView *page = [[UIStackView alloc] init];
    page.axis = UILayoutConstraintAxisVertical;
    page.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:page];

    UILayoutGuide *readable = self.view.readableContentGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [page.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:20],
        [page.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-28],
        [page.leadingAnchor constraintEqualToAnchor:readable.leadingAnchor],
        [page.trailingAnchor constraintEqualToAnchor:readable.trailingAnchor],
    ]];

    // Header
    UILabel *title = [self labelWithStyle:UIFontTextStyleLargeTitle color:[UIColor labelColor]];
    UIFontDescriptor *large = [UIFontDescriptor preferredFontDescriptorWithTextStyle:UIFontTextStyleLargeTitle];
    title.font = [UIFont fontWithDescriptor:[large fontDescriptorWithSymbolicTraits:UIFontDescriptorTraitBold] ?: large size:0];
    title.text = @"InputLine";
    title.accessibilityTraits = UIAccessibilityTraitHeader;
    [page addArrangedSubview:title];
    [page setCustomSpacing:4 afterView:title];
    UILabel *subtitle = [self labelWithStyle:UIFontTextStyleSubheadline color:[UIColor secondaryLabelColor]];
    subtitle.text = @"Your Steam Controller with full Steam Input on your PC, next to any streaming app.";
    [page addArrangedSubview:subtitle];
    [page setCustomSpacing:28 afterView:subtitle];

    [self buildPCCard:[self addCardTo:page title:@"PC"]];
    [self buildControllerCard:[self addCardTo:page title:@"Controller"]];
    [self buildDiagnosticsCard:[self addCardTo:page title:@"Diagnostics"]];

    // Footer
    UIStackView *footer = [[UIStackView alloc] init];
    footer.axis = UILayoutConstraintAxisVertical;
    footer.spacing = 8;
    footer.layoutMarginsRelativeArrangement = YES;
    footer.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(0, 16, 0, 16);
    [footer addArrangedSubview:[self note:@"Leave InputLine in the background while you play; don't swipe it away in the app switcher. "
                                           "While no controller is on, iOS pauses InputLine, and switching the controller on wakes it up."]];
    NSDictionary *info = [NSBundle mainBundle].infoDictionary;
    NSString *version = info[@"InputLineVersion"] ?: info[@"CFBundleShortVersionString"] ?: @"";
    UILabel *versionLabel = [self note:[NSString stringWithFormat:@"InputLine %@", version]];
    versionLabel.textColor = [UIColor tertiaryLabelColor];
    [footer addArrangedSubview:versionLabel];
    [page addArrangedSubview:footer];

    if (_screenshotState == nil) {
        [ILNBridge shared].delegate = self;
    }
    [self refresh];
}

- (void)buildPCCard:(UIStackView *)card
{
    _pcRow = [[ILNStatusRow alloc] init];
    [card addArrangedSubview:_pcRow];

    _pcDetail = [self note:@""];
    _pcDetail.hidden = YES;
    [card addArrangedSubview:_pcDetail];

    _updateLabel = [self labelWithStyle:UIFontTextStyleFootnote color:ILNOrange()];
    _updateLabel.hidden = YES;
    [card addArrangedSubview:_updateLabel];

    _addressField = [[UITextField alloc] init];
    _addressField.placeholder = @"PC address, e.g. 192.168.1.20";
    _addressField.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _addressField.adjustsFontForContentSizeCategory = YES;
    _addressField.backgroundColor = [UIColor tertiarySystemFillColor];
    _addressField.layer.cornerRadius = 10;
    _addressField.layer.cornerCurve = kCACornerCurveContinuous;
    _addressField.leftView = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 12, 1)];
    _addressField.leftViewMode = UITextFieldViewModeAlways;
    _addressField.clearButtonMode = UITextFieldViewModeWhileEditing;
    _addressField.keyboardType = UIKeyboardTypeURL;
    _addressField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    _addressField.autocorrectionType = UITextAutocorrectionTypeNo;
    _addressField.returnKeyType = UIReturnKeyGo;
    _addressField.delegate = self;
    _addressField.text = _screenshotState == nil ? [ILNBridge shared].status.pcAddress : @"";
    [_addressField.heightAnchor constraintGreaterThanOrEqualToConstant:46].active = YES;
    [card addArrangedSubview:_addressField];
    [card setCustomSpacing:10 afterView:_addressField];

    _connectButton = [self buttonWithAction:@selector(connectOrDisconnect)];
    [self styleButton:_connectButton title:@"Connect" style:ILNButtonStylePrimary];
    [card addArrangedSubview:_connectButton];

    _discoveredStack = [[UIStackView alloc] init];
    _discoveredStack.axis = UILayoutConstraintAxisVertical;
    _discoveredStack.spacing = 8;
    _discoveredStack.hidden = YES;
    [card addArrangedSubview:_discoveredStack];

    _pcTip = [self note:@"The PC needs InputLine installed. At home, InputLine finds it on the network by itself. "
                         "Away from home, enter its address on your VPN (for example Tailscale)."];
    [card addArrangedSubview:_pcTip];
}

- (void)buildControllerCard:(UIStackView *)card
{
    _controllerRows = [[UIStackView alloc] init];
    _controllerRows.axis = UILayoutConstraintAxisVertical;
    _controllerRows.spacing = 14;
    [card addArrangedSubview:_controllerRows];

    _pauseButton = [self buttonWithAction:@selector(togglePaused)];
    [self styleButton:_pauseButton title:@"Disconnect from PC" style:ILNButtonStyleDestructive];
    _pauseButton.hidden = YES;
    [card addArrangedSubview:_pauseButton];
    [card setCustomSpacing:10 afterView:_pauseButton];

    _pauseNote = [self note:[NSString stringWithFormat:@"The controller works with this %@ now. Tap Connect to PC, or switch the controller off and on, to send it back.",
                                                       [self deviceName]]];
    _pauseNote.hidden = YES;
    [card addArrangedSubview:_pauseNote];

    _pairButton = [self buttonWithAction:@selector(pairController)];
    [self styleButton:_pairButton title:@"Pair a new controller" style:ILNButtonStyleSecondary];
    [card addArrangedSubview:_pairButton];
    [card setCustomSpacing:10 afterView:_pairButton];

    _pairNote = [self note:@"Put the controller in Bluetooth pairing mode first, then tap the button and accept the pairing request. "
                            "Paired controllers reconnect by themselves when you switch them on."];
    [card addArrangedSubview:_pairNote];
}

- (void)buildDiagnosticsCard:(UIStackView *)card
{
    _shareButton = [self buttonWithAction:@selector(shareReport)];
    [self styleButton:_shareButton title:@"Share report" style:ILNButtonStyleSecondary];
    UIButton *copy = [self buttonWithAction:@selector(copyReport)];
    [self styleButton:copy title:@"Copy" style:ILNButtonStyleSecondary];
    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[_shareButton, copy]];
    buttons.spacing = 10;
    buttons.distribution = UIStackViewDistributionFillEqually;
    [card addArrangedSubview:buttons];
    [card setCustomSpacing:10 afterView:buttons];
    [card addArrangedSubview:[self note:@"When something goes wrong, share the report along with the PC's log."]];

    _detailsButton = [self buttonWithAction:@selector(toggleDetails)];
    [card addArrangedSubview:_detailsButton];

    _detailsStack = [[UIStackView alloc] init];
    _detailsStack.axis = UILayoutConstraintAxisVertical;
    _detailsStack.spacing = 8;
    [_detailsStack addArrangedSubview:[self heading:@"Bluetooth timing"]];
    [_detailsStack addArrangedSubview:[self note:@"How evenly controller reports arrive. About 67 per second (one every 15 ms) is normal on iPad and iPhone. "
                                                  "Play a while with your streaming app in front, then come back to compare."]];
    _timingNowLabel = [self monospaced:UIFontTextStyleFootnote];
    _timingForegroundLabel = [self monospaced:UIFontTextStyleFootnote];
    _timingBackgroundLabel = [self monospaced:UIFontTextStyleFootnote];
    for (UILabel *label in @[_timingNowLabel, _timingForegroundLabel, _timingBackgroundLabel]) {
        [_detailsStack addArrangedSubview:label];
    }
    UIButton *reset = [self buttonWithAction:@selector(resetTiming)];
    [self styleButton:reset title:@"Reset timing" style:ILNButtonStyleLink];
    [_detailsStack addArrangedSubview:reset];
    [_detailsStack setCustomSpacing:16 afterView:reset];
    [_detailsStack addArrangedSubview:[self heading:@"Recent events"]];
    _eventsLabel = [self monospaced:UIFontTextStyleCaption1];
    _eventsLabel.textColor = [UIColor secondaryLabelColor];
    [_detailsStack addArrangedSubview:_eventsLabel];
    [card addArrangedSubview:_detailsStack];

    const BOOL expanded = [[NSUserDefaults standardUserDefaults] boolForKey:kDiagnosticsExpandedKey];
    _detailsStack.hidden = !expanded;
    [self styleDetailsButton:expanded];
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

#pragma mark - Refresh

- (ILNStatus *)currentStatus
{
    return _screenshotState != nil ? [self screenshotStatus:_screenshotState] : [ILNBridge shared].status;
}

- (void)refresh
{
    ILNStatus *status = [self currentStatus];
    [self refreshPC:status];
    [self refreshControllers:status];
    [self refreshDiagnostics:status];
    [self applyLayoutChanges];
}

- (void)applyLayoutChanges
{
    if (_layoutChanges.count == 0) {
        return;
    }
    NSArray<void (^)(void)> *changes = [_layoutChanges copy];
    [_layoutChanges removeAllObjects];
    void (^apply)(void) = ^{
        for (void (^change)(void) in changes) {
            change();
        }
        [self.view layoutIfNeeded];
    };
    if (self.view.window != nil && !UIAccessibilityIsReduceMotionEnabled()) {
        [UIView animateWithDuration:0.3
                              delay:0
             usingSpringWithDamping:1
              initialSpringVelocity:0
                            options:UIViewAnimationOptionBeginFromCurrentState
                         animations:apply
                         completion:nil];
    } else {
        [UIView performWithoutAnimation:apply];
    }
}

- (void)refreshPC:(ILNStatus *)status
{
    NSString *name = status.pcName.length > 0 ? status.pcName : status.pcAddress.length > 0 ? status.pcAddress : @"Your PC";
    NSString *detail = @"";
    switch (status.linkState) {
        case ILNLinkStateNoPC:
            [_pcRow setSymbol:@"desktopcomputer" title:@"Your PC" status:@"Not set up yet" color:ILNGray() busy:NO];
            detail = @"Choose your PC below, or enter its address.";
            break;
        case ILNLinkStateSearching:
            [_pcRow setSymbol:@"desktopcomputer" title:name status:@"Looking for the PC" color:ILNOrange() busy:YES];
            break;
        case ILNLinkStateNotFound:
            [_pcRow setSymbol:@"desktopcomputer" title:name status:@"Not answering, still trying" color:ILNRed() busy:NO];
            detail = status.problemText;
            break;
        case ILNLinkStatePairing:
            [_pcRow setSymbol:@"desktopcomputer" title:name status:@"Waiting for the pairing code" color:ILNOrange() busy:NO];
            detail = @"Enter the 6-digit code shown on the PC.";
            break;
        case ILNLinkStateConnecting:
            [_pcRow setSymbol:@"desktopcomputer" title:name status:status.linkUp ? @"Reconnecting" : @"Connecting" color:ILNOrange() busy:YES];
            break;
        case ILNLinkStateConnected: {
            NSMutableString *text = [NSMutableString stringWithString:@"Connected"];
            if (status.rttMs >= 0) {
                [text appendFormat:status.rttMs < 10 ? @" · %.1f ms" : @" · %.0f ms", status.rttMs];
            }
            if (status.viaTailscale) {
                [text appendString:@" · Tailscale"];
            }
            [_pcRow setSymbol:@"desktopcomputer" title:name status:text color:ILNGreen() busy:NO];
            break;
        }
        case ILNLinkStateDisconnected:
            [_pcRow setSymbol:@"desktopcomputer" title:name status:@"Disconnected" color:ILNRed() busy:NO];
            break;
    }
    _pcDetail.text = detail;
    [self setView:_pcDetail visible:detail.length > 0];
    _updateLabel.text = status.updateText;
    [self setView:_updateLabel visible:status.updateText.length > 0];

    [self setView:_addressField visible:!status.linkUp];
    [self styleButton:_connectButton
                title:status.linkUp ? @"Disconnect" : @"Connect"
                style:status.linkUp ? ILNButtonStyleDestructive : ILNButtonStylePrimary];
    [self showDiscoveredPCs:status];
    const BOOL lookingForPC = status.linkState == ILNLinkStateNoPC || status.linkState == ILNLinkStateSearching || status.linkState == ILNLinkStateNotFound;
    [self setView:_pcTip visible:lookingForPC];
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
    [self setView:_discoveredStack visible:others.count > 0 && !status.linkUp];
    if ([key isEqualToString:_discoveredShown ?: @""]) {
        return;
    }
    _discoveredShown = key;
    for (UIView *view in _discoveredStack.arrangedSubviews) {
        [view removeFromSuperview];
    }
    if (others.count > 0) {
        [_discoveredStack addArrangedSubview:[self note:@"Found on this network"]];
    }
    NSMutableArray<NSString *> *addresses = [NSMutableArray array];
    for (ILNDiscoveredPC *pc in others) {
        UIButtonConfiguration *config = [UIButtonConfiguration grayButtonConfiguration];
        config.title = pc.name;
        config.subtitle = pc.address;
        config.image = [UIImage systemImageNamed:@"desktopcomputer"];
        config.imagePadding = 12;
        config.titleAlignment = UIButtonConfigurationTitleAlignmentLeading;
        config.cornerStyle = UIButtonConfigurationCornerStyleLarge;
        config.contentInsets = NSDirectionalEdgeInsetsMake(12, 14, 12, 14);
        config.baseForegroundColor = [UIColor labelColor];
        UIButton *button = [self buttonWithAction:@selector(useDiscoveredPC:)];
        button.configuration = config;
        button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
        button.accessibilityHint = @"Connects to this PC";
        button.tag = (NSInteger)addresses.count;
        [addresses addObject:pc.address];
        [_discoveredStack addArrangedSubview:button];
    }
    _discoveredAddresses = addresses;
}

- (void)refreshControllers:(ILNStatus *)status
{
    NSArray<ILNControllerInfo *> *controllers = status.controllerInfo;
    const NSUInteger rows = MAX(controllers.count, (NSUInteger)1);
    while (_controllerRows.arrangedSubviews.count < rows) {
        [_controllerRows addArrangedSubview:[[ILNStatusRow alloc] init]];
    }
    while (_controllerRows.arrangedSubviews.count > rows) {
        [_controllerRows.arrangedSubviews.lastObject removeFromSuperview];
    }
    if (controllers.count == 0) {
        [(ILNStatusRow *)_controllerRows.arrangedSubviews.firstObject setSymbol:@"gamecontroller"
                                                                          title:@"No controller"
                                                                         status:@"Switch it on to connect"
                                                                          color:ILNGray()
                                                                           busy:NO];
        [(ILNStatusRow *)_controllerRows.arrangedSubviews.firstObject setBatteryLevel:-1 charging:NO];
    }
    NSString *pc = status.pcName.length > 0 ? status.pcName : @"the PC";
    for (NSUInteger i = 0; i < controllers.count; ++i) {
        ILNStatusRow *row = (ILNStatusRow *)_controllerRows.arrangedSubviews[i];
        NSString *title = controllers.count > 1 ? [NSString stringWithFormat:@"Steam Controller %lu", (unsigned long)i + 1] : @"Steam Controller";
        switch (controllers[i].state) {
            case ILNControllerStateOnPC:
                [row setSymbol:@"gamecontroller.fill" title:title status:[NSString stringWithFormat:@"Connected to %@", pc] color:ILNGreen() busy:NO];
                break;
            case ILNControllerStateConnecting:
                [row setSymbol:@"gamecontroller.fill" title:title status:[NSString stringWithFormat:@"Connecting to %@", pc] color:ILNOrange() busy:YES];
                break;
            case ILNControllerStateWaitingForPC:
                [row setSymbol:@"gamecontroller.fill" title:title status:@"Waiting for the PC" color:ILNOrange() busy:NO];
                break;
            case ILNControllerStateOnThisDevice:
                [row setSymbol:@"gamecontroller.fill" title:title status:@"Disconnected from PC" color:ILNRed() busy:NO];
                break;
        }
        [row setBatteryLevel:controllers[i].batteryLevel charging:controllers[i].batteryCharging];
    }

    const BOOL any = controllers.count > 0;
    [self setView:_pauseButton visible:any && status.linkState != ILNLinkStateDisconnected];
    [self styleButton:_pauseButton
                title:status.paused ? @"Connect to PC" : @"Disconnect from PC"
                style:status.paused ? ILNButtonStylePrimary : ILNButtonStyleDestructive];
    [self setView:_pauseNote visible:any && status.paused];
    // Pairing matters until a controller is on; after that it's a quiet link.
    [self styleButton:_pairButton
                title:any ? @"Pair another controller" : @"Pair a new controller"
                style:any ? ILNButtonStyleLink : ILNButtonStyleSecondary];
    [self setView:_pairNote visible:!any];
}

- (void)refreshDiagnostics:(ILNStatus *)status
{
    _timingNowLabel.text = [NSString stringWithFormat:@"Now         %@", status.timingNow.length > 0 ? status.timingNow : @"-"];
    _timingForegroundLabel.text = [NSString stringWithFormat:@"Foreground  %@", status.timingForeground.length > 0 ? status.timingForeground : @"-"];
    _timingBackgroundLabel.text = [NSString stringWithFormat:@"Background  %@", status.timingBackground.length > 0 ? status.timingBackground : @"-"];
    NSArray<NSString *> *recent = status.events.count > 12 ? [status.events subarrayWithRange:NSMakeRange(status.events.count - 12, 12)] : status.events;
    _eventsLabel.text = recent.count > 0 ? [recent componentsJoinedByString:@"\n"] : @"-";
}

- (void)styleDetailsButton:(BOOL)expanded
{
    UIButtonConfiguration *config = [UIButtonConfiguration plainButtonConfiguration];
    config.title = expanded ? @"Hide timing and events" : @"Show timing and events";
    config.image = [UIImage systemImageNamed:expanded ? @"chevron.up" : @"chevron.down"];
    config.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote scale:UIImageSymbolScaleSmall];
    config.imagePlacement = NSDirectionalRectEdgeTrailing;
    config.imagePadding = 6;
    config.contentInsets = NSDirectionalEdgeInsetsMake(4, 0, 4, 0);
    _detailsButton.configuration = config;
    _detailsButton.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
}

#pragma mark - Screenshots

/// Made-up states for screenshots (see ILNScreenshotKey).
- (ILNStatus *)screenshotStatus:(NSString *)state
{
    ILNStatus *status = [[ILNStatus alloc] init];
    status.pcName = @"LIVING-ROOM-PC";
    status.pcAddress = @"192.168.1.20";
    status.linkText = @"";
    status.updateText = @"";
    status.problemText = @"";
    status.rttMs = -1;
    status.controllers = @[];
    status.controllerInfo = @[];
    status.discoveredPCs = @[];
    status.timingNow = @"";
    status.timingForeground = @"";
    status.timingBackground = @"";
    status.timingSent = @"";
    status.events = @[];
    ILNControllerInfo *controller = [[ILNControllerInfo alloc] init];
    controller.name = @"Steam Controller";
    controller.batteryLevel = -1;
    if ([state isEqualToString:@"setup"]) {
        status.linkState = ILNLinkStateNoPC;
        status.pcName = @"";
        status.pcAddress = @"";
        status.discoveredPCs = @[
            [ILNDiscoveredPC pcWithName:@"LIVING-ROOM-PC" address:@"192.168.1.20"],
            [ILNDiscoveredPC pcWithName:@"STUDIO" address:@"192.168.1.31"],
        ];
    } else if ([state isEqualToString:@"searching"]) {
        status.linkState = ILNLinkStateSearching;
    } else if ([state isEqualToString:@"notfound"]) {
        status.linkState = ILNLinkStateNotFound;
        status.problemText = @"No answer from 192.168.1.20. Is inputline-host running there, and is UDP 48150 allowed through its firewall? Still trying.";
    } else if ([state isEqualToString:@"disconnected"]) {
        status.linkState = ILNLinkStateDisconnected;
        controller.state = ILNControllerStateOnThisDevice;
        controller.batteryLevel = 36;
        controller.batteryCharging = YES;
        status.controllerInfo = @[controller];
    } else {  // connected
        status.linkState = ILNLinkStateConnected;
        status.linkUp = YES;
        status.rttMs = 2.4;
        controller.state = ILNControllerStateOnPC;
        controller.batteryLevel = 82;
        status.controllerInfo = @[controller];
    }
    return status;
}

#pragma mark - Actions

- (void)togglePaused
{
    if (_screenshotState != nil) {
        return;
    }
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

- (void)connectOrDisconnect
{
    if (_screenshotState != nil) {
        return;
    }
    if ([ILNBridge shared].status.linkUp) {
        [[ILNBridge shared] disconnectFromPC];
        [self refresh];
        return;
    }
    [self connect];
}

- (void)connect
{
    [_addressField resignFirstResponder];
    if (_screenshotState != nil) {
        return;
    }
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
    if (_screenshotState != nil) {
        return;
    }
    [[ILNBridge shared] scanForNewControllers];
    [self bridge:[ILNBridge shared] showMessage:@"Looking for controllers in pairing mode for 30 seconds."];
}

- (void)toggleDetails
{
    const BOOL expand = _detailsStack.hidden;
    [[NSUserDefaults standardUserDefaults] setBool:expand forKey:kDiagnosticsExpandedKey];
    [self styleDetailsButton:expand];
    [self setView:_detailsStack visible:expand];
    [self applyLayoutChanges];
}

- (void)shareReport
{
    NSString *report = [[ILNBridge shared] report];
    // Shared as a file, so Save to Files and mail attachments get a proper name.
    NSDateFormatter *format = [[NSDateFormatter alloc] init];
    format.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    format.dateFormat = @"yyyy-MM-dd HH.mm";
    NSString *name = [NSString stringWithFormat:@"InputLine report %@.txt", [format stringFromDate:[NSDate date]]];
    NSURL *file = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES] URLByAppendingPathComponent:name];
    NSArray *items = [report writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nil] ? @[file] : @[report];
    UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:items applicationActivities:nil];
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
    UIAlertAction *pair = [UIAlertAction actionWithTitle:@"Pair" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        [[ILNBridge shared] submitPairingCode:weakAlert.textFields.firstObject.text ?: @""];
    }];
    [alert addAction:pair];
    alert.preferredAction = pair;

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
