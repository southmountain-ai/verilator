// -*- mode: C++; c-file-style: "cc-mode" -*-
//*****************************************************************************
// DESCRIPTION: Verilator: Semantic trace provenance tags shared across passes
//
// Code available from: https://verilator.org
//
//*****************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2003-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*****************************************************************************

#ifndef VERILATOR_V3SEMANTICTRACETAG_H_
#define VERILATOR_V3SEMANTICTRACETAG_H_

#include "config_build.h"
#include "verilatedos.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace V3SemanticTraceTag {

enum class UpdateKind : uint8_t { Pending, Commit };

struct Provenance {
    std::string sourceProcessId;
    std::string sourceKind;
    std::string signalName;
    std::string scheme;
    std::string sourceFile;
    std::string realCppName;
    int sourceLine = 0;
    UpdateKind updateKind = UpdateKind::Pending;
};

// Keys are stable synthesized variable names (e.g. "__Vdly__foo", "foo").
inline std::unordered_map<std::string, Provenance> g_pendingByShadowName;
inline std::unordered_map<std::string, Provenance> g_commitByRealName;

inline void clear() {
    g_pendingByShadowName.clear();
    g_commitByRealName.clear();
}

inline void setPending(const std::string& shadowName, Provenance tag) {
    g_pendingByShadowName[shadowName] = std::move(tag);
}

inline const Provenance* getPending(const std::string& shadowName) {
    const auto it = g_pendingByShadowName.find(shadowName);
    if (it == g_pendingByShadowName.end()) return nullptr;
    return &it->second;
}

inline void setCommit(const std::string& realName, Provenance tag) {
    g_commitByRealName[realName] = std::move(tag);
}

inline const Provenance* getCommit(const std::string& realName) {
    const auto it = g_commitByRealName.find(realName);
    if (it == g_commitByRealName.end()) return nullptr;
    return &it->second;
}

}  // namespace V3SemanticTraceTag

#endif  // Guard
