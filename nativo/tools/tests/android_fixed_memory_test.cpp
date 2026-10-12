// Run on-device with the Android runtime, including old kernels that ignore
// MAP_FIXED_NOREPLACE. Recommit mirrors reuse of guest thread guard pages.
#include <rex/memory.h>
#include <cstdio>
#include <sys/mman.h>

int main() {
  using namespace rex::memory;
  const size_t length = page_size();
  void* reserved = mmap(nullptr, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reserved == MAP_FAILED) return 1;
  void* committed = AllocFixed(reserved, length, AllocationType::kCommit, PageAccess::kReadWrite);
  if (committed != reserved) {
    std::printf("FAIL: fixed commit returned %p instead of %p\n", committed, reserved);
    return 1;
  }
  auto bytes = static_cast<volatile unsigned char*>(reserved);
  bytes[0] = 0x35;
  bytes[length - 1] = 0xA8;
  if (AllocFixed(reserved, length, AllocationType::kReserve, PageAccess::kNoAccess) != nullptr ||
      bytes[0] != 0x35 || bytes[length - 1] != 0xA8) {
    std::puts("FAIL: colliding reservation replaced an existing mapping");
    return 1;
  }
  if (!Protect(reserved, length, PageAccess::kNoAccess, nullptr) ||
      AllocFixed(reserved, length, AllocationType::kCommit, PageAccess::kReadWrite) != reserved) {
    std::puts("FAIL: protected mapping could not be recommitted");
    return 1;
  }
  if (bytes[0] != 0x35 || bytes[length - 1] != 0xA8) {
    std::puts("FAIL: recommit lost existing contents");
    return 1;
  }
  bytes[0] = 0xBE;
  bytes[length - 1] = 0xBE;
  munmap(reserved, length);
  std::puts("PASS: fixed commit, collision protection and guard-page reuse");
}
