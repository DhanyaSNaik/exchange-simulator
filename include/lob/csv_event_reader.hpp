#pragma once
// Synthetic CSV order-flow format (Phase 3). Columns, in order:
//   ts_ns,event,order_id,side,type,price,qty,ref_id
//
// NEW:     order_id,side,type,price,qty set; ref_id blank.
// CANCEL:  order_id set; everything else blank.
// REPLACE: order_id set; price and/or qty set (blank = unchanged, maps
//          to std::nullopt); ref_id blank.
// TRADE:   price and qty are the execution's price/quantity; ref_id is
//          the RESTING order being hit; order_id is blank (no aggressor
//          to name -- see replay_event.hpp / DESIGN_DECISIONS.md).
// TICK:    only ts_ns set; everything else blank.
//
// A header row is REQUIRED and validated against this exact column
// order -- a wrong/missing header fails immediately rather than
// silently misreading columns. Any malformed row (wrong column count,
// unparseable number, unrecognized event kind, a value present where
// the event kind requires it blank or vice versa) is a FILE-FORMAT
// problem, not a data-quality one, and is ALWAYS fatal -- this is
// intentionally not governed by SimulationLoop's strict_mode flag (which
// is about what to do when an otherwise well-formed event can't be
// cleanly applied to the book, e.g. a TRADE whose ref_id isn't resting).
// Conflating "the file is malformed" with "this one row's data doesn't
// match current book state" would hide a whole-file problem behind a
// flag meant for row-level issues.

#include <stdexcept>
#include <string>
#include <vector>

#include "lob/replay_event.hpp"

namespace lob {

struct CsvParseError : std::runtime_error {
    std::size_t line_number;  // 1-indexed, including the header
    CsvParseError(std::size_t line, const std::string& msg)
        : std::runtime_error("CSV parse error at line " + std::to_string(line) + ": " + msg),
          line_number(line) {}
};

// Parses the full CSV content (not a file path -- callers read the file
// themselves, keeping this testable without touching a filesystem).
// Throws CsvParseError on ANY malformed row -- see the file-level
// comment above for why this is unconditional. Returns events in file
// order; file_order is assigned 0, 1, 2, ... as rows are read, NOT
// re-derived from timestamp (two events can share a timestamp; file
// order is what breaks that tie, so it must reflect actual row order,
// not a sort of it).
std::vector<OrderFlowEvent> parseCsvEvents(const std::string& csv_content);

}  // namespace lob
