// Compiles Metal shader source files with the system's Metal framework and reports
// errors. Used to check translated shaders without Xcode's separate Metal toolchain.
//
//   clang -fobjc-arc -framework Foundation -framework Metal tools/mslcheck.m -o build/mslcheck
//   build/mslcheck FILE.metal ...        (or: build/mslcheck --list FILE_WITH_PATHS)
//
// Prints one line per failing file and a total; exit status 1 if any failed.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(int argc, const char ** argv)
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            NSArray<id<MTLDevice>> * all = MTLCopyAllDevices();
            device = all.firstObject;
        }
        if (!device)
        {
            fprintf(stderr, "mslcheck: no Metal device is available\n");
            return 2;
        }
        NSMutableArray<NSString *> * paths = [NSMutableArray array];
        if (argc == 3 && strcmp(argv[1], "--list") == 0)
        {
            NSString * text = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil];
            for (NSString * line in [text componentsSeparatedByString:@"\n"])
            {
                if (line.length)
                {
                    [paths addObject:line];
                }
            }
        }
        else
        {
            for (int i = 1; i < argc; i++)
            {
                [paths addObject:@(argv[i])];
            }
        }
        int failed = 0;
        for (NSString * path in paths)
        {
            NSString * source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            NSError * error = nil;
            id<MTLLibrary> library = source ? [device newLibraryWithSource:source options:nil error:&error] : nil;
            if (!library)
            {
                failed++;
                NSString * message = error.localizedDescription ?: @"could not read file";
                NSArray<NSString *> * lines = [message componentsSeparatedByString:@"\n"];
                NSString * first = lines.firstObject ?: @"";
                for (NSString * l in lines)
                {
                    if ([l containsString:@"error:"])
                    {
                        first = l;
                        break;
                    }
                }
                printf("FAIL %s: %s\n", path.UTF8String, first.UTF8String);
            }
        }
        printf("%lu compiled, %d failed\n", (unsigned long)paths.count - failed, failed);
        return failed ? 1 : 0;
    }
}
