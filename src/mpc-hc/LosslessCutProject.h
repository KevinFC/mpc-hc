/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-HC.
 *
 * MPC-HC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-HC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

// LosslessCut project (.llc) model, JSON5 reader/writer and crash-safe file IO.
//
// Deliberately free of MFC so it can be unit tested on its own. Everything is UTF-8
// internally; paths are wide strings.
//
// Format (LosslessCut 3.x, "version: 2"), written by JSON5.stringify(project, null, 2):
//   { version: 2, mediaFileName: 'x.mp4', cutSegments: [ { start, end?, name, tags?, selected? } ] }
// Version 1 files are accepted (missing start = 0) and upgraded to version 2 on the first write,
// exactly like LosslessCut does. Unknown keys, segment tags and key order are preserved.

#include <windows.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llc
{
    struct JsonValue {
        enum class Type { Null, Bool, Number, String, Array, Object };
        Type type = Type::Null;
        bool b = false;
        double num = 0.0;
        std::string str;                                         // UTF-8
        std::vector<JsonValue> arr;
        std::vector<std::pair<std::string, JsonValue>> obj;      // insertion ordered

        static JsonValue MakeNumber(double v) { JsonValue j; j.type = Type::Number; j.num = v; return j; }
        static JsonValue MakeString(std::string v) { JsonValue j; j.type = Type::String; j.str = std::move(v); return j; }
        static JsonValue MakeBool(bool v) { JsonValue j; j.type = Type::Bool; j.b = v; return j; }
        static JsonValue MakeObject() { JsonValue j; j.type = Type::Object; return j; }
        static JsonValue MakeArray() { JsonValue j; j.type = Type::Array; return j; }

        const JsonValue* Find(const char* key) const;
        JsonValue* Find(const char* key);
        void Set(const char* key, JsonValue v);   // replaces in place, or appends at the end
        void Erase(const char* key);
        bool operator==(const JsonValue& o) const;
        bool operator!=(const JsonValue& o) const { return !(*this == o); }
    };

    // Strict JSON5 parser (comments, trailing commas, single quotes, identifier keys, hex,
    // Infinity/NaN). Duplicate keys are rejected so an ambiguous file is never rewritten.
    bool ParseJson5(const std::string& utf8, JsonValue& out, std::string& error);

    // Byte-compatible with json5's JSON5.stringify(value, null, 2).
    std::string StringifyJson5(const JsonValue& value);

    // ECMAScript Number.prototype.toString() for finite doubles (shortest round-trip).
    std::string FormatJsNumber(double v);

    struct Segment {
        double start = 0.0;              // seconds
        std::optional<double> end;       // seconds; no end => a LosslessCut "marker"
        std::string name;                // UTF-8
        bool selected = true;            // included in the export
        JsonValue raw;                   // original object, keeps tags and unknown keys

        bool IsMarker() const { return !end.has_value(); }
        bool SameContent(const Segment& o) const {
            return start == o.start && end == o.end && name == o.name && selected == o.selected;
        }
    };

    class Project
    {
    public:
        enum class LoadStatus { Ok, ParseError, UnsupportedVersion, InvalidSchema };

        // "<dir>\<name without extension>-proj.llc", the name LosslessCut looks for next to the media.
        static std::wstring ProjectPathForMedia(const std::wstring& mediaPath);

        void InitNew(const std::wstring& mediaPath);
        LoadStatus Load(const std::string& bytes, std::string& error);

        // Merges m_segments back into the original document and renders it.
        std::string Serialize() const;

        // Parses the given bytes and checks that they describe exactly these segments.
        bool VerifySerialized(const std::string& bytes, std::string& error) const;

        std::vector<Segment> segments;
        bool upgradedFromV1 = false;

    private:
        JsonValue m_doc;
        static bool ExtractSegments(const JsonValue& doc, std::vector<Segment>& out, int& version, std::string& error);
    };

    // Crash-safe file IO for small files.
    namespace FileIO
    {
        struct Stamp {
            bool exists = false;
            ULONGLONG size = 0;
            FILETIME lastWrite = {};
            bool operator==(const Stamp& o) const {
                return exists == o.exists && size == o.size &&
                       lastWrite.dwLowDateTime == o.lastWrite.dwLowDateTime &&
                       lastWrite.dwHighDateTime == o.lastWrite.dwHighDateTime;
            }
            bool operator!=(const Stamp& o) const { return !(*this == o); }
        };

        Stamp GetStamp(const std::wstring& path);
        bool ReadAll(const std::wstring& path, std::string& out, DWORD& winError);

        // Writes to "<path>.mpchc-tmp" in the same folder, flushes it to disk, atomically swaps it
        // over the target (ReplaceFileW keeps the target's attributes/ACL; MoveFileEx is the
        // fallback), then reads the target back and compares. The target is never truncated or
        // partially written: on any failure it keeps its previous content.
        bool AtomicWrite(const std::wstring& path, const std::string& bytes, std::wstring& error);

        // Copies the file into backupDir as "<name>.<yyyyMMdd-HHmmss>.llc" and keeps the newest maxKeep.
        bool Backup(const std::wstring& path, const std::wstring& backupDir, int maxKeep, std::wstring& error);
    }

    std::wstring Utf8ToWide(const std::string& s);
    std::string WideToUtf8(const std::wstring& s);
}
