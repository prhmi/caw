#pragma once
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// A patch is a separate file, not the clip's code field (which is a path).
// Keep a copy of what was loaded so Save refuses to overwrite external edits.
namespace pd_patch_editor {
constexpr std::uintmax_t maxBytes = 4 * 1024 * 1024;
struct Buffer {
    std::string text, original, message;
    std::string readable, readableBase, readableMessage;
    bool loaded = false;
    bool dirty() const { return loaded && (text != original || readable != readableBase); }
};
struct ReadableRow {
    std::string prefix, content, suffix;
    std::size_t start=0, length=0;
};
inline std::string escapeDollarArguments(const std::string& text) {
    std::string result;result.reserve(text.size());
    for(std::size_t i=0;i<text.size();++i) {
        if(text[i]=='$' && i+1<text.size() && text[i+1]>='0' && text[i+1]<='9') {
            std::size_t slashes=0,j=i;while(j>0&&text[j-1]=='\\'){--j;++slashes;}
            if(slashes%2==0)result+='\\';
        }
        result+=text[i];
    }
    return result;
}
// Only single-line object/message/comment records are editable. Everything
// else remains byte-for-byte in the source, including all connect records.
inline std::vector<ReadableRow> readableRows(const std::string& source) {
    static const std::regex pattern(R"pd(^(#X[ \t]+(obj|msg|text)[ \t]+[-+]?[0-9]+[ \t]+[-+]?[0-9]+[ \t]+)(.*)(;)(\r?)$)pd");
    std::vector<ReadableRow> rows;
    std::size_t pos=0;
    while(pos<source.size()) {
        auto end=source.find('\n',pos);
        if(end==std::string::npos)end=source.size();
        const std::string raw=source.substr(pos,end-pos);
        std::smatch m;
        if(std::regex_match(raw,m,pattern)) {
            ReadableRow row;
            row.prefix=m[1].str();row.content=m[3].str();
            row.suffix=m[4].str()+m[5].str();row.start=pos;row.length=end-pos;
            rows.push_back(std::move(row));
        }
        pos=end<source.size()?end+1:end;
    }
    return rows;
}
inline std::string readableText(const std::string& source) {
    std::string result;
    for(const auto& r:readableRows(source))result+=r.content+";\n";
    return result;
}
inline bool applyReadable(const std::string& source,const std::string& draft,std::string& output,std::string& error) {
    const auto rows=readableRows(source);
    std::istringstream input(draft);
    std::vector<std::string> edits;
    std::string line;
    for(const auto& row:rows) {
        if(!std::getline(input,line)) {
            error="Keep the same number of lines; edit patch structure in Pure Data.";return false;
        }
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty() || line.back()!=';') {
            error="End each line with a semicolon; edit patch structure in Pure Data.";return false;
        }
        std::size_t trailingSlashes=0, j=line.size()-1;
        while(j>0 && line[j-1]=='\\'){--j;++trailingSlashes;}
        if(trailingSlashes%2!=0) {
            error="Each line needs an unescaped final semicolon.";return false;
        }
        std::string content=line.substr(0,line.size()-1);
        if(content.find('\r')!=std::string::npos || content.find('\0')!=std::string::npos) {
            error="Invalid control character in an object line.";return false;
        }
        if(content.find_first_not_of(" \t")==std::string::npos && content!=row.content) {
            error="Keep each object, message or comment line non-empty.";return false;
        }
        // An unescaped semicolon would terminate a PD record and could alter
        // connections. Existing escaped punctuation remains exactly as-is.
        for(std::size_t i=0;i<content.size();++i)if(content[i]==';') {
            std::size_t slashes=0,j=i;
            while(j>0 && content[j-1]=='\\'){--j;++slashes;}
            if(slashes%2==0){error="Escape semicolons in object text as \\;.";return false;}
        }
        // Newly typed $0/$1 references need PD serialization escaping.
        edits.push_back(content==row.content?content:escapeDollarArguments(content));
    }
    // Blank lines at the end are harmless (the text editor shows an empty last line); any other extra line is a new row.
    while(std::getline(input,line)) {
        if(line.find_first_not_of(" \t\r")!=std::string::npos) {
            error="Don't add lines here; use Pure Data to change patch structure.";return false;
        }
    }
    output=source;
    for(std::size_t i=rows.size();i>0;--i) {
        const auto& row=rows[i-1];
        if(edits[i-1]!=row.content)
            output.replace(row.start,row.length,row.prefix+edits[i-1]+row.suffix);
    }
    error.clear();return true;
}
// A non-destructive index into the original text, not a parser or a patch
// serializer. Source line numbers are not PD object/connection indices.
struct SourceItem { std::size_t line; std::string kind, content; };
inline std::vector<SourceItem> sourceItems(const std::string& text, std::size_t limit, bool& truncated) {
    std::vector<SourceItem> items;
    std::istringstream input(text);
    std::string line;
    std::size_t lineNo = 0;
    truncated = false;
    while (std::getline(input, line)) {
        ++lineNo;
        std::istringstream tokens(line);
        std::string marker, kind, x, y;
        if (!(tokens >> marker >> kind >> x >> y) || marker != "#X") continue;
        if (kind != "obj" && kind != "msg" && kind != "text" && kind != "floatatom" &&
            kind != "symbolatom" && kind != "listbox" && kind != "restore") continue;
        std::string content;
        std::getline(tokens, content);
        auto start = content.find_first_not_of(" \t");
        if (start != std::string::npos) content.erase(0, start);
        else content.clear();
        if (items.size() == limit) { truncated = true; break; }
        items.push_back({lineNo, kind, content});
    }
    return items;
}
inline bool read(const std::filesystem::path& path, std::string& text, std::string& error) {
    std::error_code ec;
    if (path.extension() != ".pd" || !std::filesystem::is_regular_file(path, ec) || ec) {
        error = "Choose an existing .pd file."; return false;
    }
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maxBytes) { error = "Patch cannot be read or exceeds 4 MB."; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "Cannot open the patch for reading."; return false; }
    text.assign(static_cast<std::size_t>(size), '\0');
    if (size && !in.read(&text[0], static_cast<std::streamsize>(size))) {
        error = "Could not read the complete patch."; return false;
    }
    if (text.find('\0') != std::string::npos) { error = "Patch contains binary data."; return false; }
    error.clear(); return true;
}
inline bool load(const std::filesystem::path& path, Buffer& b) {
    std::string data, error;
    if (!read(path, data, error)) { b.message = error; return false; }
    b.text = data; b.original = data;
    b.readable=readableText(data);b.readableBase=b.readable;b.readableMessage.clear();
    b.loaded = true; b.message.clear(); return true;
}
inline bool save(const std::filesystem::path& path, Buffer& b) {
    if (!b.loaded) { b.message = "Load the patch first."; return false; }
    std::string next = b.text;
    if (b.readable != b.readableBase) {
        std::string error;
        if(!applyReadable(b.text,b.readable,next,error)){b.message=error;return false;}
    }
    if (next.size() > maxBytes || next.find('\0') != std::string::npos) {
        b.message = "Patch text is invalid or exceeds 4 MB."; return false;
    }
    std::string current, error;
    if (!read(path, current, error)) { b.message = error; return false; }
    if (current != b.original) {
        b.message = "File changed outside CAW. Copy your edits, then discard and reload; it was not overwritten.";
        return false;
    }
    std::error_code ec;
    std::filesystem::path temp;
    for (int i = 0; i < 1000; ++i) {
        temp = path;
        temp += std::filesystem::u8path(".caw-tmp-" + std::to_string(i));
        if (!std::filesystem::exists(temp, ec) && !ec) break;
        if (ec || i == 999) { b.message = "Cannot create a temporary patch file."; return false; }
    }
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) { b.message = "Cannot write the temporary patch file."; return false; }
        out.write(next.data(), static_cast<std::streamsize>(next.size()));
        out.close();
        if (!out) { std::filesystem::remove(temp, ec); b.message = "Could not write the complete patch."; return false; }
    }
    // Recheck after writing the temporary file; never deliberately replace a
    // file which was edited externally while this buffer was open.
    if (!read(path, current, error) || current != b.original) {
        std::filesystem::remove(temp, ec); b.message = "File changed outside CAW; it was not overwritten."; return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temp, ec); b.message = "Could not replace the patch file."; return false;
    }
#else
    std::filesystem::rename(temp, path, ec);
    if (ec) { std::filesystem::remove(temp, ec); b.message = "Could not replace the patch file."; return false; }
#endif
    b.text = std::move(next);b.original = b.text;b.readable=readableText(b.text);b.readableBase=b.readable;
    b.readableMessage.clear();b.message = "Patch saved."; return true;
}
}
