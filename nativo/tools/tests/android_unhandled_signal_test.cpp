// Run with the freshly built Android runtime and its shared dependencies.
#include <rex/exception_handler.h>

#include <cstdio>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
bool Handle(rex::arch::Exception*, void* data) { return data != nullptr; }
void Previous(int, siginfo_t*, void*) { _exit(77); }

bool Run(int mode) {
  pid_t child = fork();
  if (child < 0) return false;
  if (!child) {
    struct sigaction previous{};
    previous.sa_sigaction = mode == 1 ? Previous : nullptr;
    previous.sa_flags = mode == 1 ? SA_SIGINFO : 0;
    sigemptyset(&previous.sa_mask);
    sigaction(SIGSEGV, &previous, nullptr);
    rex::arch::ExceptionHandler::Install(Handle, mode == 0 ? reinterpret_cast<void*>(1) : nullptr);
    if (mode == 0) {
      raise(SIGILL);
      _exit(0);
    }
    void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) _exit(99);
    *static_cast<volatile unsigned*>(page) = 42;
    _exit(98);  // An unhandled synchronous fault must not return here.
  }
  int status = 0;
  for (int i = 0; i < 200; ++i) {
    if (waitpid(child, &status, WNOHANG) == child) {
      if (mode == 0) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
      if (mode == 1) return WIFEXITED(status) && WEXITSTATUS(status) == 77;
      return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    }
    usleep(25000);
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  return false;
}
}  // namespace

int main() {
  for (int mode = 0; mode < 3; ++mode) {
    if (!Run(mode)) {
      std::fprintf(stderr, "Signal regression failed in mode %d\n", mode);
      return 1;
    }
  }
  std::puts("Android signals passed: handled, previous handler, default fatal handler");
}
