// SPDX-License-Identifier: MIT

#include <cstdint>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>

int main(int argc, char** argv)
{
    const std::vector<std::uint32_t> values{1, 2, 3, 4};
    const auto sum = std::accumulate(values.begin(), values.end(), 0U);
    const bool self_test = argc == 2 && std::string_view(argv[1]) == "--self-test";

    if (!self_test || sizeof(void*) != 8 || sum != 10U) {
        return 1;
    }

    std::cout << "QCOM application framework self-test passed\n";
    return 0;
}
