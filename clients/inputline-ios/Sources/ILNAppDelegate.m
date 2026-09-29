//
//  ILNAppDelegate.m
//  InputLine
//

#import "ILNAppDelegate.h"
#import "ILNBridge.h"
#import "ILNViewController.h"

@implementation ILNAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    // Also runs when iOS relaunches the app in the background to hand back a
    // controller (Bluetooth state restoration): start the bridge either way.
    if ([[NSUserDefaults standardUserDefaults] stringForKey:ILNScreenshotKey] == nil) {
        [[ILNBridge shared] start];
    }

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [[ILNViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)applicationWillTerminate:(UIApplication *)application
{
    [[ILNBridge shared] prepareForTermination];
}

@end
