#include <sanitizer/asan_interface.h>

#include <cstddef>
#include <iostream>
#include <memory>
#include <string_view>

#ifndef __SANITIZE_ADDRESS__
#error This probe must be compiled with AddressSanitizer instrumentation
#endif

// Keep invalid reads observable under optimization; this is a test-only probe.
// 中文翻译：优化时仍保留无效读取；本程序仅作测试探针。
__declspec(noinline) int readValue(const int* values, std::size_t index)
{
    return values[index];
}

int main(int argc, char** argv)
{
    if(argc != 2) { return 2; }
    const std::string_view mode{argv[1]};
    constexpr std::size_t count = 4U;
    auto values = std::make_unique<int[]>(count);
    for(std::size_t index = 0U; index < count; ++index) { values[index] = static_cast<int>(index + 1U); }
    if(__asan_address_is_poisoned(values.get()) != 0
       || __asan_address_is_poisoned(values.get() + count) == 0) {
        std::cerr << "asan-probe: unexpected shadow memory\n";
        return 2;
    }
    if(mode == "healthy") {
        if(readValue(values.get(), count - 1U) != 4) { return 2; }
        std::cout << "asan-probe-healthy\n";
        return 0;
    }
    if(mode == "heap-buffer-overflow") {
        std::cout << readValue(values.get(), count) << '\n';
        return 2;
    }
    if(mode == "heap-use-after-free") {
        const int* released = values.get();
        values.reset();
        std::cout << readValue(released, 0U) << '\n';
        return 2;
    }
    return 2;
}
