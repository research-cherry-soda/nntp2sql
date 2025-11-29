// File: /Users/jon/Development/NOV2625/fsit/main.m
//
// fsit - File System Inspector Tool for macOS
// Build:
//   clang -ObjC -fobjc-arc -framework Foundation -framework CoreServices -o fsit main.m
//
// Usage:
//   ./fsit /path/to/file
//
// Prints: POSIX stat, extended attributes, ACLs, NSURL resource values, and Spotlight (MDItem) attributes.

@import Foundation;
@import CoreServices;

#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/types.h>
#include <sys/acl.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

static void printStat(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        printf("stat: %s\n", strerror(errno));
        return;
    }
    struct passwd *pw = getpwuid(st.st_uid);
    struct group  *gr = getgrgid(st.st_gid);
    char atime[64], mtime[64], ctimebuf[64];
    strftime(atime, sizeof(atime), "%FT%T%z", localtime(&st.st_atime));
    strftime(mtime, sizeof(mtime), "%FT%T%z", localtime(&st.st_mtime));
    strftime(ctimebuf, sizeof(ctimebuf), "%FT%T%z", localtime(&st.st_ctime));

    printf("POSIX stat:\n");
    printf("  path: %s\n", path);
    printf("  inode: %llu\n", (unsigned long long)st.st_ino);
    printf("  size: %llu\n", (unsigned long long)st.st_size);
    printf("  blocks: %llu\n", (unsigned long long)st.st_blocks);
    printf("  mode: %ho (octal)\n", st.st_mode & 07777);
    printf("  uid: %u (%s)\n", st.st_uid, pw ? pw->pw_name : "unknown");
    printf("  gid: %u (%s)\n", st.st_gid, gr ? gr->gr_name : "unknown");
    printf("  atime: %s\n", atime);
    printf("  mtime: %s\n", mtime);
    printf("  ctime: %s\n", ctimebuf);
#ifdef __APPLE__
    printf("  flags: 0x%x\n", st.st_flags);
#endif
    printf("\n");
}

static void printXattrs(const char *path) {
    ssize_t bufsize = listxattr(path, NULL, 0, XATTR_NOFOLLOW);
    if (bufsize < 0) {
        if (errno == ENOTSUP) {
            printf("Extended attributes: not supported on this filesystem\n\n");
            return;
        }
        printf("listxattr: %s\n\n", strerror(errno));
        return;
    }
    if (bufsize == 0) {
        printf("Extended attributes: none\n\n");
        return;
    }
    char *buf = malloc((size_t)bufsize);
    if (!buf) return;
    ssize_t ret = listxattr(path, buf, bufsize, XATTR_NOFOLLOW);
    if (ret < 0) {
        printf("listxattr: %s\n\n", strerror(errno));
        free(buf);
        return;
    }
    printf("Extended attributes (name : size bytes):\n");
    char *p = buf;
    while (p < buf + ret) {
        size_t nameLen = strlen(p);
        ssize_t valSize = getxattr(path, p, NULL, 0, 0, XATTR_NOFOLLOW);
        if (valSize < 0) {
            printf("  %s : (error: %s)\n", p, strerror(errno));
        } else {
            printf("  %s : %zd\n", p, valSize);
        }
        p += nameLen + 1;
    }
    printf("\n");
    free(buf);
}

static void printACL(const char *path) {
    acl_t acl = acl_get_file(path, ACL_TYPE_EXTENDED);
    if (acl == NULL) {
        if (errno == ENOENT || errno == ENOTSUP) {
            printf("ACL: not present\n\n");
            return;
        }
        // On macOS, if there is no ACL, acl_get_file returns NULL and errno == 0.
        if (errno == 0) {
            printf("ACL: none\n\n");
            return;
        }
        printf("acl_get_file: %s\n\n", strerror(errno));
        return;
    }
    char *text = acl_to_text(acl, NULL);
    if (text) {
        printf("POSIX ACL (text):\n%s\n", text);
        acl_free(text);
    } else {
        printf("ACL: (present, but failed to convert to text)\n\n");
    }
    acl_free(acl);
    printf("\n");
}

static void printNSURLProperties(NSString *path) {
    NSURL *url = [NSURL fileURLWithPath:path];
    NSArray<NSURLResourceKey> *keys = @[
        NSURLNameKey,
        NSURLLocalizedNameKey,
        NSURLCreationDateKey,
        NSURLContentModificationDateKey,
        NSURLTypeIdentifierKey,
        NSURLFileSizeKey,
        NSURLFileResourceIdentifierKey,
        NSURLVolumeIdentifierKey,
        NSURLIsAliasFileKey,
        NSURLIsHiddenKey,
        NSURLIsReadableKey,
        NSURLIsWritableKey,
        NSURLIsExecutableKey,
        NSURLIsDirectoryKey,
        NSURLIsSymbolicLinkKey,
        NSURLIsPackageKey,
        NSURLTotalFileAllocatedSizeKey
    ];
    NSError *err = nil;
    NSDictionary<NSURLResourceKey,id> *vals = [url resourceValuesForKeys:keys error:&err];
    if (!vals) {
        printf("NSURL resource values: error: %s\n\n", err ? [[err localizedDescription] UTF8String] : "unknown");
        return;
    }
    printf("NSURL resource values:\n");
    for (NSURLResourceKey key in keys) {
        id v = vals[key];
        if (v == nil) continue;
        const char *ck = [[key description] UTF8String];
        NSString *s = [v description];
        printf("  %s: %s\n", ck, [s UTF8String]);
    }
    printf("\n");
}

static void printMDItemAttributes(NSString *path) {
    CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:path];
    MDItemRef item = MDItemCreateWithURL(kCFAllocatorDefault, url);
    if (!item) {
        printf("Spotlight/MDItem: none or failed to create\n\n");
        return;
    }
    CFArrayRef names = MDItemCopyAttributeNames(item);
    if (!names) {
        CFRelease(item);
        printf("MDItem attribute names: none\n\n");
        return;
    }
    CFIndex count = CFArrayGetCount(names);
    printf("Spotlight (MDItem) attributes (%ld):\n", (long)count);
    for (CFIndex i = 0; i < count; ++i) {
        CFStringRef key = CFArrayGetValueAtIndex(names, i);
        CFTypeRef value = MDItemCopyAttribute(item, key);
        char keybuf[256];
        if (CFStringGetCString(key, keybuf, sizeof(keybuf), kCFStringEncodingUTF8)) {
            if (value) {
                CFStringRef valDesc = CFCopyDescription(value);
                char valbuf[2048] = {0};
                if (valDesc && CFStringGetCString(valDesc, valbuf, sizeof(valbuf), kCFStringEncodingUTF8)) {
                    printf("  %s: %s\n", keybuf, valbuf);
                } else {
                    printf("  %s: (value present)\n", keybuf);
                }
                if (valDesc) CFRelease(valDesc);
                CFRelease(value);
            } else {
                printf("  %s: (null)\n", keybuf);
            }
        }
    }
    CFRelease(names);
    CFRelease(item);
    printf("\n");
}

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        if (argc < 2) {
            printf("Usage: %s <path>\n", argv[0]);
            return 1;
        }
        const char *path = argv[1];
        NSString *pathStr = [NSString stringWithUTF8String:path];

        printStat(path);
        printXattrs(path);
        printACL(path);
        printNSURLProperties(pathStr);
        printMDItemAttributes(pathStr);

        // Note: This tool prints many common attributes. Some filesystem-specific metadata
        // (e.g. APFS snapshots, undocumented flags) may require privileged APIs or kernel calls.
    }
    return 0;
}