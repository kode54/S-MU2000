// license:BSD-3-Clause
//
// AUv3 の入口。.appex の NSExtensionPrincipalClass がこれ。
// システムはここに「音源を 1 台作れ」と頼んでくる。

#import "audio_unit.h"

#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

@interface SMU2000Factory : NSObject <AUAudioUnitFactory>
@end

@implementation SMU2000Factory

// NSExtensionRequestHandling。AUv3 では何もしなくてよい
- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context
{
	(void)context;
}

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error
{
	return [[SMU2000AudioUnit alloc] initWithComponentDescription:desc error:error];
}

@end
