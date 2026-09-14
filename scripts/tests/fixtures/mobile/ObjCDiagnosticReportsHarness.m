#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

@interface NDDiagnosticReports : NSObject
- (uint64_t)initializer;
- (uint64_t)initializerInFile;
- (uint64_t)fatal;
- (uint64_t)fatalInFile;
- (void)terminal;
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(int argc, char **argv) {
  struct rlimit core = {0, 0};
  if (setrlimit(RLIMIT_CORE, &core))
    return 2;
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDDiagnosticReports *object = [NDDiagnosticReports new];
    if (argc == 2 && !strcmp(argv[1], "--trap")) {
      [object terminal];
      return 3;
    }
    if ([object initializer] != 42 || [object initializerInFile] != 91 ||
        [object fatal] != 137 || [object fatalInFile] != 251)
      return 4;
    [object release];
    pid_t pid = fork();
    if (pid < 0)
      return 5;
    if (!pid) {
      execl(argv[0], argv[0], "--trap", NULL);
      _exit(6);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status))
      return 7;
#if defined(__arm64__)
    if (WTERMSIG(status) != SIGTRAP)
      return 8;
#else
    if (WTERMSIG(status) != SIGILL)
      return 8;
#endif
    puts("diagnostic-runtime=pass\ncontents=pass\ntrap=pass");
  }
}
