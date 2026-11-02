// Links every generated function (through the function table) and reports the table's size. Runs nothing.
#include "stub_runtime.h"

#include <cstdio>

#define BOZO_CAT2(a, b) a##b
#define BOZO_CAT(a, b) BOZO_CAT2(a, b)
extern const BozoFunctionEntry BOZO_CAT(BOZO_PREFIX, _function_table)[];
extern const size_t BOZO_CAT(BOZO_PREFIX, _function_count);

int main()
{
    const BozoFunctionEntry *table = BOZO_CAT(BOZO_PREFIX, _function_table);
    const size_t count = BOZO_CAT(BOZO_PREFIX, _function_count);
    size_t sorted = 1;
    for (size_t i = 1; i < count; ++i)
        sorted &= table[i - 1].address < table[i].address;
    std::printf("function table: %zu entries, %s\n", count, sorted ? "sorted" : "NOT SORTED");
    return sorted ? 0 : 1;
}
