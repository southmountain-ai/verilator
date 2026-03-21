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
/// where each line is one trace record capturing the simulator's causal
/// execution model: which regions fired and why, which processes ran in what
/// order, and what tentative next-state values were computed.
///
/// Design principle: every field must help a model understand WHY the
/// simulator executed something and HOW it reached the next state.
/// Signal value dumps belong in VCD; scheduler causality belongs here.
///
/// Record types (see trace_schema/trace_schema.md for full spec):
///   sim_start      — emitted once at simulation start
///   sim_end        — emitted once at simulation end
///   active_region  — one iteration of the active scheduling region
///   nba_region     — one iteration of the NBA scheduling region
///
/// Fields added to region records for scheduler causality:
///   iteration      — 0-indexed loop iteration within the current time step
///   trigger_count  — number of trigger bits that fired (popcount of trigger_vec)
///   trigger_vec    — raw trigger vector word 0 as hex (which processes are sensitive)
///
/// Fields added to per-process records:
///   process_index  — execution order within the region (0 = first)
///
//=============================================================================

#ifndef VERILATOR_VERILATED_SEMANTIC_TRACE_H_
#define VERILATOR_VERILATED_SEMANTIC_TRACE_H_

#include <cassert>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

//=============================================================================
// VerilatedSemanticTrace
//
// One instance per simulation run.  Not thread-safe (Verilator single-eval
// mode only; MT support can be added later).
//=============================================================================

class VerilatedSemanticTrace final {
    // =========================================================================
    // Internal state
    // =========================================================================
    std::ofstream m_out;              // Output JSONL file
    uint64_t      m_time = 0;         // Current simulation time
    uint32_t      m_delta = 0;        // Current delta cycle within time step
    bool          m_inActive = false; // Inside an active_region record?
    bool          m_inNba    = false; // Inside an nba_region record?
    bool          m_inProcess = false; // Inside a process record?
    bool          m_firstProcess = true;  // JSON comma guard for processes array
    bool          m_firstNbaWrite = true;  // JSON comma guard for nba_writes array

    // Trigger map: index i holds the human-readable description of trigger bit i.
    // Populated by registerTrigger() before simStart() is called.
    std::vector<std::string> m_actTrigDescs;

    // Scheduler causality counters
    uint64_t m_actIteration = 0;  // Active region iterations at current time step
    uint64_t m_nbaIteration = 0;  // NBA region iterations at current time step
    uint32_t m_processIndex = 0;  // Execution order of processes within current region

    // =========================================================================
    // JSON helpers (no external dependency)
    // =========================================================================

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

    static std::string hexVal(uint64_t v) {
        std::ostringstream ss;
        ss << "\"0x" << std::hex << v << '"';
        return ss.str();
    }

    // Portable popcount for trigger_count field
    static uint32_t popcount64(uint64_t v) {
        v = v - ((v >> 1) & 0x5555555555555555ULL);
        v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
        v = (v + (v >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
        return static_cast<uint32_t>((v * 0x0101010101010101ULL) >> 56);
    }

public:
    // =========================================================================
    // Construction / destruction
    // =========================================================================

    explicit VerilatedSemanticTrace(const std::string& filename) {
        m_out.open(filename, std::ios::out | std::ios::trunc);
        if (!m_out.is_open()) {
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

    // Register the human-readable description for a trigger bit index.
    // Must be called BEFORE simStart() so the map appears in the sim_start record.
    //
    // LIMITATION: only bits 0–63 (word 0 of the trigger vector) are captured.
    // Designs with >64 unique sensitivities will have bits >=64 registered here but
    // their values silently dropped from trigger_vec (which only records word 0).
    //
    // To extend to multi-word support:
    //   1. Change activeRegionStart / nbaRegionStart to accept
    //      (uint64_t time, const uint64_t* trigWords, uint32_t nWords).
    //   2. Emit trigger_vec as a JSON array: ["0x...", "0x..."].
    //   3. Update VL_SEMANTIC_TRACE_ACTIVE_START / NBA_START macros accordingly.
    //   4. In V3Sched.cpp, pass &triggered[0] and trigKit.m_nVecWords instead of [0U].
    //   The trigger_map bit indices are already absolute, so no schema change needed there.
    void registerTrigger(uint32_t bit, const char* desc) {
        if (bit >= 64) {
            fprintf(stderr,
                    "%%Warning: --semantic-trace: trigger bit %u is in word %u (beyond word 0); "
                    "its value will not appear in trigger_vec (only 64 bits captured). "
                    "See verilated_semantic_trace.h for multi-word extension notes.\n",
                    bit, bit / 64);
        }
        if (bit >= static_cast<uint32_t>(m_actTrigDescs.size()))
            m_actTrigDescs.resize(bit + 1);
        m_actTrigDescs[bit] = desc;
    }

    void simStart(const std::string& designId) {
        m_out << "{\"type\":\"sim_start\","
              << "\"design_id\":" << jsonStr(designId);
        if (!m_actTrigDescs.empty()) {
            m_out << ",\"trigger_map\":{";
            bool first = true;
            for (uint32_t i = 0; i < static_cast<uint32_t>(m_actTrigDescs.size()); ++i) {
                if (m_actTrigDescs[i].empty()) continue;
                if (!first) m_out << ",";
                m_out << "\"" << i << "\":" << jsonStr(m_actTrigDescs[i]);
                first = false;
            }
            m_out << "}";
        }
        m_out << "}\n";
    }

    void simEnd() {
        m_out << "{\"type\":\"sim_end\","
              << "\"time\":" << m_time << "}\n";
        m_out.flush();
    }

    // =========================================================================
    // Active region
    // =========================================================================

    // Call before the active work executes in _eval_phase__act().
    //   time       — current simulation time (vlSymsp->_vm_contextp__->time())
    //   trigWord   — first word of the act trigger vector (which sensitivities fired)
    void activeRegionStart(uint64_t time, uint64_t trigWord) {
        assert(!m_inActive);
        m_inActive = true;
        m_firstProcess = true;
        m_processIndex = 0;

        if (time != m_time) {
            // New time step: reset all per-step counters
            m_time = time;
            m_delta = 0;
            m_actIteration = 0;
            m_nbaIteration = 0;
        }

        m_out << "{\"type\":\"active_region\","
              << "\"time\":" << m_time << ","
              << "\"delta\":" << m_delta << ","
              << "\"iteration\":" << m_actIteration << ","
              << "\"trigger_count\":" << popcount64(trigWord) << ","
              << "\"trigger_vec\":" << hexVal(trigWord) << ","
              << "\"processes\":[";

        ++m_actIteration;
    }

    // Call after the active work completes in _eval_phase__act().
    void activeRegionEnd() {
        assert(m_inActive);
        m_inActive = false;
        m_out << "]}\n";
    }

    // =========================================================================
    // Per-process records (emitted during active or NBA region)
    // =========================================================================

    // Called just before entering a generated process function body.
    //   processId    — source location e.g. "always_ff@fifo.sv:59"
    //   kind         — "always_ff", "always_comb", "always", etc.
    //   trigger      — sensitivity string (currently "clocked"; full decoding is future work)
    void processStart(const char* processId, const char* kind, const char* trigger) {
        // Skip calls outside any tracked region (initial, static, final, settle, etc.)
        if (!m_inActive && !m_inNba) return;
        assert(!m_inProcess);
        m_inProcess = true;

        if (!m_firstProcess) m_out << ',';
        m_firstProcess = false;

        m_out << "{\"process_id\":" << jsonStr(processId) << ","
              << "\"process_index\":" << m_processIndex << ","
              << "\"kind\":" << jsonStr(kind) << ","
              << "\"trigger\":" << jsonStr(trigger) << ","
              << "\"nba_writes\":[";
        m_firstNbaWrite = true;
        ++m_processIndex;
    }

    // Called after the process function body returns.
    void processEnd() {
        if (!m_inProcess) return;  // processStart was skipped (outside tracked region)
        m_inProcess = false;
        m_out << "]}";
    }

    // Called inside a process body when a top-level __Vdly__ variable is written
    // (i.e., a nonblocking assignment has been evaluated in the active-region pass).
    //   signal         — RTL signal name (without __Vdly__ prefix)
    //   tentativeValue — the value just written to the shadow variable
    //   currentValue   — the pre-commit value of the real signal
    void nbaPending(const char* signal, uint64_t tentativeValue, uint64_t currentValue) {
        if (!m_inProcess) return;

        if (!m_firstNbaWrite) m_out << ',';
        m_firstNbaWrite = false;

        m_out << "{\"signal\":" << jsonStr(signal) << ","
              << "\"tentative\":" << hexVal(tentativeValue) << ","
              << "\"current\":" << hexVal(currentValue) << "}";
    }

    // =========================================================================
    // NBA region
    // =========================================================================

    // Call before the NBA work executes in _eval_phase__nba().
    //   trigWord — first word of the nba trigger vector (which processes are pending)
    void nbaRegionStart(uint64_t trigWord) {
        assert(!m_inNba);
        m_inNba = true;
        m_firstProcess = true;
        m_processIndex = 0;
        ++m_delta;

        m_out << "{\"type\":\"nba_region\","
              << "\"time\":" << m_time << ","
              << "\"delta\":" << m_delta << ","
              << "\"iteration\":" << m_nbaIteration << ","
              << "\"trigger_count\":" << popcount64(trigWord) << ","
              << "\"trigger_vec\":" << hexVal(trigWord) << ","
              << "\"processes\":[";

        ++m_nbaIteration;
    }

    // Call after the NBA work completes in _eval_phase__nba().
    void nbaRegionEnd() {
        assert(m_inNba);
        m_inNba = false;
        m_out << "]}\n";
        ++m_delta;
    }
};

//=============================================================================
// Convenience macros used in generated code
//
// All macros are no-ops when semantic tracing is disabled (pointer is nullptr).
// The compiler eliminates the branches entirely at -O2.
//=============================================================================

#define VL_SEMANTIC_TRACE_ACTIVE_START(tracep, time, trigger_word) \
    do { if (tracep) (tracep)->activeRegionStart(time, trigger_word); } while (false)

#define VL_SEMANTIC_TRACE_ACTIVE_END(tracep) \
    do { if (tracep) (tracep)->activeRegionEnd(); } while (false)

#define VL_SEMANTIC_TRACE_PROCESS_START(tracep, id, kind, trigger) \
    do { if (tracep) (tracep)->processStart(id, kind, trigger); } while (false)

#define VL_SEMANTIC_TRACE_PROCESS_END(tracep) \
    do { if (tracep) (tracep)->processEnd(); } while (false)

#define VL_SEMANTIC_TRACE_NBA_PENDING(tracep, sig, tentative, current) \
    do { if (tracep) (tracep)->nbaPending(sig, tentative, current); } while (false)

#define VL_SEMANTIC_TRACE_NBA_START(tracep, trigger_word) \
    do { if (tracep) (tracep)->nbaRegionStart(trigger_word); } while (false)

#define VL_SEMANTIC_TRACE_NBA_END(tracep) \
    do { if (tracep) (tracep)->nbaRegionEnd(); } while (false)

#define VL_SEMANTIC_TRACE_REGISTER_TRIGGER(tracep, bit, desc) \
    do { if (tracep) (tracep)->registerTrigger(bit, desc); } while (false)

#endif  // VERILATOR_VERILATED_SEMANTIC_TRACE_H_
