// -*- mode: C++; c-file-style: "cc-mode" -*-
//=============================================================================
//
// Code available from: https://verilator.org
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2001-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//=============================================================================
///
/// \file
/// \brief Semantic execution trace runtime for Verilator
///
/// When a design is compiled with --semantic-trace <file>, the generated
/// simulation model includes calls to this class.  It writes a JSONL file
/// where each line is one trace record capturing scheduler-level semantics:
/// which processes fired, what nonblocking assignments they queued (tentative
/// values in the active region), and what was committed in the NBA region.
///
/// Record types (see trace_schema/trace_schema.md for full spec):
///   sim_start      — emitted once at simulation start
///   sim_end        — emitted once at simulation end
///   active_region  — start of active scheduling region for a time step
///   process_start  — one always_ff / always_comb / assign process firing
///   process_end    — end of that process
///   nba_write      — tentative NBA write queued during active region
///   nba_region     — NBA flush: all __Vdly__ commits for this time step
///   nba_commit     — one signal committed during NBA region
///
//=============================================================================

#ifndef VERILATOR_VERILATED_SEMANTIC_TRACE_H_
#define VERILATOR_VERILATED_SEMANTIC_TRACE_H_

#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

//=============================================================================
// VerilatedSemanticTrace
//
// One instance per simulation run.  Not thread-safe (Verilator single-eval
// mode only for now; MT support can be added later).
//=============================================================================

class VerilatedSemanticTrace final {
    // =========================================================================
    // Internal state
    // =========================================================================
    std::ofstream m_out;              // Output JSONL file
    uint64_t      m_time = 0;         // Current simulation time
    uint32_t      m_delta = 0;        // Current delta cycle within time step
    bool          m_inActive = false; // Are we inside an active region record?
    bool          m_inNba    = false; // Are we inside an NBA region record?
    bool          m_inProcess = false; // Are we inside a process record?
    bool          m_firstProcess = true;  // For JSON array comma handling
    bool          m_firstCommit  = true;  // For JSON array comma handling

    uint32_t m_nbaWriteCount = 0;  // count of NBA writes queued this active region

    // =========================================================================
    // JSON helpers (no external dependency)
    // =========================================================================

    // Escape a string for JSON (handles \, ", and control chars)
    static std::string jsonStr(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out += '"';
        for (char c : s) {
            if (c == '"')       { out += "\\\""; }
            else if (c == '\\') { out += "\\\\"; }
            else if (c == '\n') { out += "\\n"; }
            else if (c == '\r') { out += "\\r"; }
            else if (c == '\t') { out += "\\t"; }
            else                { out += c; }
        }
        out += '"';
        return out;
    }

    static std::string jsonStr(const char* s) {
        return jsonStr(std::string(s ? s : ""));
    }

    // Format a uint64 value as a hex string (e.g. "0x1f")
    static std::string hexVal(uint64_t v) {
        std::ostringstream ss;
        ss << "\"0x" << std::hex << v << '"';
        return ss.str();
    }

    void emit(const std::string& line) {
        m_out << line << '\n';
    }

public:
    // =========================================================================
    // Construction / destruction
    // =========================================================================

    explicit VerilatedSemanticTrace(const std::string& filename) {
        m_out.open(filename, std::ios::out | std::ios::trunc);
        if (!m_out.is_open()) {
            // Mimic Verilator fatal pattern; in practice the generated code
            // checks this at startup.
            fprintf(stderr, "%%Error: Cannot open semantic trace file: %s\n",
                    filename.c_str());
        }
    }

    ~VerilatedSemanticTrace() {
        if (m_out.is_open()) m_out.flush();
    }

    // =========================================================================
    // Simulation lifecycle
    // =========================================================================

    void simStart(const std::string& designId) {
        emit("{\"type\":\"sim_start\","
             "\"design_id\":" + jsonStr(designId) + "}");
    }

    void simEnd() {
        emit("{\"type\":\"sim_end\","
             "\"time\":" + std::to_string(m_time) + "}");
        m_out.flush();
    }

    // =========================================================================
    // Active region
    // =========================================================================

    // Call before _eval_phase__act().
    // Pass the current simulation time so the record is correctly time-stamped.
    // Resets the delta counter when time advances.
    void activeRegionStart(uint64_t time) {
        assert(!m_inActive);
        m_inActive = true;
        m_firstProcess = true;
        m_nbaWriteCount = 0;

        if (time != m_time) {
            m_time = time;
            m_delta = 0;
        }

        // Begin the active_region record; process array opened here.
        m_out << "{\"type\":\"active_region\","
              << "\"time\":" << m_time << ","
              << "\"delta\":" << m_delta << ","
              << "\"processes\":[";
    }

    // Call after _eval_phase__act().
    void activeRegionEnd() {
        assert(m_inActive);
        m_inActive = false;

        // Close process array; add pending NBA writes summary.
        m_out << "],"
              << "\"nba_writes_queued\":" << m_nbaWriteCount
              << "}\n";
    }

    // =========================================================================
    // Per-process records (emitted during active or NBA region)
    // =========================================================================

    // Called just before entering a generated process function body.
    //   processId — source location string e.g. "always_ff@fifo.sv:38"
    //   kind      — "always_ff", "always_comb", "assign", etc.
    //   trigger   — sensitivity expression e.g. "posedge clk"
    void processStart(const char* processId, const char* kind,
                      const char* trigger) {
        // processStart/End are injected into region subfunctions by both
        // orderSequentially and V3OrderCFuncEmitter.  Skip calls that fire
        // outside any tracked region (initial, static, final, settle, etc.).
        if (!m_inActive && !m_inNba) return;
        assert(!m_inProcess);
        m_inProcess = true;

        if (!m_firstProcess) m_out << ',';
        m_firstProcess = false;

        m_out << "{\"process_id\":" << jsonStr(processId) << ","
              << "\"kind\":"       << jsonStr(kind)      << ","
              << "\"trigger\":"    << jsonStr(trigger)   << ","
              << "\"nba_writes\":[";
        m_firstCommit = true;  // reuse for inner nba_writes array
    }

    // Called after the process function body returns.
    void processEnd() {
        if (!m_inProcess) return;  // processStart was skipped (outside active region)
        m_inProcess = false;
        m_out << "]}";
    }

    // Called inside a process body when a __Vdly__ variable is written
    // (i.e., a nonblocking assignment has been evaluated).
    //   signal         — RTL signal name (without __Vdly__ prefix)
    //   tentativeValue — the value just written to __Vdly__
    //   currentValue   — the current (pre-NBA-commit) value of the signal
    void nbaPending(const char* signal, uint64_t tentativeValue,
                    uint64_t currentValue) {
        assert(m_inProcess);

        if (!m_firstCommit) m_out << ',';
        m_firstCommit = false;

        m_out << "{\"signal\":"    << jsonStr(signal) << ","
              << "\"tentative\":"  << hexVal(tentativeValue) << ","
              << "\"current\":"    << hexVal(currentValue)   << "}";

        ++m_nbaWriteCount;
    }

    // =========================================================================
    // NBA region
    // =========================================================================

    // Call before _eval_phase__nba().
    void nbaRegionStart() {
        assert(!m_inNba);
        m_inNba = true;
        m_firstProcess = true;
        m_firstCommit = true;
        ++m_delta;

        m_out << "{\"type\":\"nba_region\","
              << "\"time\":" << m_time << ","
              << "\"delta\":" << m_delta << ","
              << "\"processes\":[";
    }

    // Call after _eval_phase__nba().
    void nbaRegionEnd() {
        assert(m_inNba);
        m_inNba = false;
        m_out << "],\"committed\":[]}\n";
        ++m_delta;
    }

    // Called just before each `real_signal = __Vdly__signal` commit.
    //   signal       — RTL signal name
    //   oldVal       — current value of the real signal (before commit)
    //   newVal       — value in the __Vdly__ shadow (about to be committed)
    //   sourceProcess — process_id that queued this write ("always_ff@...")
    void nbaCommit(const char* signal, uint64_t oldVal, uint64_t newVal,
                   const char* sourceProcess) {
        assert(m_inNba);

        if (!m_firstCommit) m_out << ',';
        m_firstCommit = false;

        m_out << "{\"signal\":"         << jsonStr(signal)        << ","
              << "\"old\":"             << hexVal(oldVal)          << ","
              << "\"new\":"             << hexVal(newVal)          << ","
              << "\"source_process\":"  << jsonStr(sourceProcess)  << "}";
    }
};

//=============================================================================
// Convenience macros used in generated code
//
// These are no-ops when semantic tracing is disabled (pointer is nullptr).
// The compiler will eliminate the branches entirely at -O2.
//=============================================================================

#define VL_SEMANTIC_TRACE_ACTIVE_START(tracep, time) \
    do { if (tracep) (tracep)->activeRegionStart(time); } while (false)

#define VL_SEMANTIC_TRACE_ACTIVE_END(tracep) \
    do { if (tracep) (tracep)->activeRegionEnd(); } while (false)

#define VL_SEMANTIC_TRACE_PROCESS_START(tracep, id, kind, trigger) \
    do { if (tracep) (tracep)->processStart(id, kind, trigger); } while (false)

#define VL_SEMANTIC_TRACE_PROCESS_END(tracep) \
    do { if (tracep) (tracep)->processEnd(); } while (false)

#define VL_SEMANTIC_TRACE_NBA_PENDING(tracep, sig, tentative, current) \
    do { if (tracep) (tracep)->nbaPending(sig, tentative, current); } while (false)

#define VL_SEMANTIC_TRACE_NBA_START(tracep) \
    do { if (tracep) (tracep)->nbaRegionStart(); } while (false)

#define VL_SEMANTIC_TRACE_NBA_END(tracep) \
    do { if (tracep) (tracep)->nbaRegionEnd(); } while (false)

#define VL_SEMANTIC_TRACE_NBA_COMMIT(tracep, sig, old_val, new_val, proc) \
    do { if (tracep) (tracep)->nbaCommit(sig, old_val, new_val, proc); } while (false)

#endif  // VERILATOR_VERILATED_SEMANTIC_TRACE_H_
