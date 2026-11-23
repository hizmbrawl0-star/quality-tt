#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <os/log.h>

#include "QualityPatch.hpp"
#include <cstdlib>
#include <exception>

namespace {
using PickerCallback = void (*)(id, SEL, id, id);
constexpr NSUInteger Tag = 478036;
NSString *const CachePrefix = @"TikTokQuality-";

void record(NSString *message) {
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "[TikTokQuality] %{public}@", message);
}

UIViewController *visibleController() {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class] ||
            scene.activationState != UISceneActivationStateForegroundActive) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            if (!window.isKeyWindow) continue;
            UIViewController *controller = window.rootViewController;
            while (controller.presentedViewController) controller = controller.presentedViewController;
            return controller;
        }
    }
    return nil;
}

void onSelected(id target, SEL selector, id picker, id file, PickerCallback original) {
    const auto forward = ^{ original(target, selector, picker, file); };
    if (!NSThread.isMainThread) {
        record(@"Обработчик вызван вне главного потока. Передаю исходный файл.");
        forward();
        return;
    }
    NSURL *source = nil;
    if ([file isKindOfClass:NSURL.class]) source = file;
    if ([file isKindOfClass:NSString.class]) source = [NSURL fileURLWithPath:file];
    if (!source.isFileURL || [source.lastPathComponent hasPrefix:CachePrefix]) {
        forward();
        return;
    }
    UIViewController *presenter = visibleController();
    if (!presenter || [presenter isKindOfClass:UIAlertController.class]) {
        record(@"Экран обработки недоступен. Передаю исходный файл.");
        forward();
        return;
    }

    NSURL *destination = [NSURL fileURLWithPath:[NSTemporaryDirectory()
        stringByAppendingPathComponent:[NSString stringWithFormat:@"%@%@.mp4",
            CachePrefix, NSUUID.UUID.UUIDString]]];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Подготовка видео"
        message:@"" preferredStyle:UIAlertControllerStyleAlert];
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    const auto addLine = ^(NSString *message) {
        record(message);
        [lines addObject:message];
        alert.message = [lines componentsJoinedByString:@"\n"];
    };
    addLine([NSString stringWithFormat:@"Выбран файл (%@)", NSStringFromSelector(selector)]);
    [presenter presentViewController:alert animated:YES completion:^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                int lastPercent = -1;
                NSString *failure = nil;
                try {
                    patchVideo(source.fileSystemRepresentation, destination.fileSystemRepresentation,
                        [&](const std::string& message) {
                            NSString *line = [NSString stringWithUTF8String:message.c_str()];
                            dispatch_async(dispatch_get_main_queue(), ^{ addLine(line); });
                        },
                        [&](uint64_t written, uint64_t total) {
                            const int percent = static_cast<int>(100.0 * written / total);
                            if (percent == lastPercent) return;
                            const bool logProgress = lastPercent < 0 || percent == 100 ||
                                percent / 10 > lastPercent / 10;
                            lastPercent = percent;
                            dispatch_async(dispatch_get_main_queue(), ^{
                                alert.title = [NSString stringWithFormat:@"Подготовка видео · %d%%", percent];
                                if (logProgress) record([NSString stringWithFormat:@"Записано %llu из %llu байт (%d%%)",
                                    (unsigned long long)written, (unsigned long long)total, percent]);
                            });
                        });
                } catch (const std::exception& error) {
                    failure = [NSString stringWithUTF8String:error.what()];
                    [NSFileManager.defaultManager removeItemAtURL:destination error:nullptr];
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (failure) {
                        alert.title = @"Ошибка подготовки";
                        addLine(failure);
                        [alert addAction:[UIAlertAction actionWithTitle:@"Продолжить с оригиналом"
                            style:UIAlertActionStyleDefault handler:^(UIAlertAction *){ forward(); }]];
                        return;
                    }
                    [alert dismissViewControllerAnimated:YES completion:^{
                        record(@"Передаю обработанный MP4 штатному обработчику TikTok.");
                        id argument = [file isKindOfClass:NSURL.class] ? (id)destination : destination.path;
                        original(target, selector, picker, argument);
                    }];
                });
            }
        });
    }];
}

bool isObjectParameter(Method method, unsigned index) {
    char *type = method_copyArgumentType(method, index);
    const bool valid = type && type[0] == '@';
    std::free(type);
    return valid;
}

void install() {
    if ([NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"] integerValue] != Tag) {
        record(@"Пропускаю сборку TikTok с другим номером версии.");
        return;
    }
    const SEL selectors[] = {
        sel_registerName("imageMTPickerController:didFinishPickingWithVideoFile:"),
        sel_registerName("ttImagePickerController:didFinishPickingWithVideoFile:")
    };
    unsigned hooked = 0, classCount = 0;
    Class __unsafe_unretained *classes = objc_copyClassList(&classCount);
    for (unsigned i = 0; i < classCount; ++i) {
        Class cls = classes[i];
        const char *image = class_getImageName(cls);
        if (!image || ![[NSString stringWithUTF8String:image] hasSuffix:@"/MusicallyCore"]) continue;
        unsigned methodCount = 0;
        Method *methods = class_copyMethodList(cls, &methodCount);
        for (unsigned j = 0; j < methodCount; ++j) {
            Method method = methods[j];
            SEL selector = method_getName(method);
            if (selector != selectors[0] && selector != selectors[1]) continue;
            char returnType[8] = {};
            method_getReturnType(method, returnType, sizeof(returnType));
            if (returnType[0] != 'v' || method_getNumberOfArguments(method) != 4 ||
                !isObjectParameter(method, 2) || !isObjectParameter(method, 3)) continue;
            auto original = reinterpret_cast<PickerCallback>(method_getImplementation(method));
            IMP replacement = imp_implementationWithBlock(^(id target, id picker, id file) {
                onSelected(target, selector, picker, file, original);
            });
            method_setImplementation(method, replacement);
            ++hooked;
            record([NSString stringWithFormat:@"Подключено %s · %@", class_getName(cls),
                                               NSStringFromSelector(selector)]);
        }
        std::free(methods);
    }
    std::free(classes);
    record([NSString stringWithFormat:@"Подключено обработчиков: %u", hooked]);
}
} // namespace

__attribute__((constructor)) static void initializeQualityTweak() {
    dispatch_async(dispatch_get_main_queue(), ^{ install(); });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSURL *folder = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
        NSFileManager *files = NSFileManager.defaultManager;
        NSArray<NSURL *> *entries = [files contentsOfDirectoryAtURL:folder
            includingPropertiesForKeys:@[NSURLContentModificationDateKey]
            options:NSDirectoryEnumerationSkipsHiddenFiles error:nullptr];
        NSDate *cutoff = [NSDate dateWithTimeIntervalSinceNow:-24 * 3600];
        for (NSURL *entry in entries) {
            if (![entry.lastPathComponent hasPrefix:CachePrefix] ||
                ![entry.pathExtension isEqualToString:@"mp4"]) continue;
            id modified = nil;
            if ([entry getResourceValue:&modified forKey:NSURLContentModificationDateKey error:nullptr] &&
                [modified isKindOfClass:NSDate.class] && [(NSDate *)modified compare:cutoff] == NSOrderedAscending) {
                [files removeItemAtURL:entry error:nullptr];
            }
        }
    });
}
