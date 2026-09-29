//
//  CLKAppDelegate.m
//  Couchlink
//

#import "CLKAppDelegate.h"
#import "CLKBridge.h"
#import "CLKViewController.h"

@implementation CLKAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    // Also runs when iOS relaunches the app in the background to hand back a
    // controller (Bluetooth state restoration): start the bridge either way.
    [[CLKBridge shared] start];

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [[CLKViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
