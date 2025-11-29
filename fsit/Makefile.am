# Automake file for fsit - File System Inspector Tool for macOS
# Requires macOS (Foundation/CoreServices frameworks) and clang/Objective-C support.

# Define the program to build.
bin_PROGRAMS = fsit

# Source files.
fsit_SOURCES = main.m

# Objective-C compiler flags.
# -fobjc-arc: enable ARC
fsit_OBJCFLAGS = -fobjc-arc

# Link against required macOS frameworks.
fsit_LDFLAGS = -framework Foundation -framework CoreServices

# (Optional) Add warnings: uncomment if desired
# AM_OBJCFLAGS = -Wall -Wextra -Werror

# Install the binary into the standard prefix bin directory.
# No extra installation rules needed.
