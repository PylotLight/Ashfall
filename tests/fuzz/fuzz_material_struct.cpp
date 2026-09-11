// Task F2: structured validator fuzzing for MaterialTable.
// Fills the struct from byte slices (Movement forged via
// static_cast<Movement>(byte)) and runs validate_material_table. The bar is
// totality: accept-or-reject with no crash, hang, or escaping exception
// (an escape is a finding). -max_len=512.

#include <cstddef>
#include <cstdint>
#include <string>

#include "ashfall/material_data.hpp"

#include "structured_fill.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    ashfall::SliceReader reader{data, size};
    ashfall::MaterialTable table;
    ashfall::fill_material_table(reader, table);
    std::string error;
    (void)ashfall::validate_material_table(table, error);
    return 0;
}
