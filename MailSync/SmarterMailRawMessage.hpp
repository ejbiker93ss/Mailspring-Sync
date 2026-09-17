#ifndef SmarterMailRawMessage_hpp
#define SmarterMailRawMessage_hpp

#include <cctype>
#include <string>

namespace SmarterMailRawMessage {

inline size_t firstContentOffset(const std::string & value) {
    size_t offset = 0;

    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        offset = 3;
    }

    while (offset < value.size() &&
           std::isspace(static_cast<unsigned char>(value[offset]))) {
        offset++;
    }
    return offset;
}

inline bool startsWithCaseInsensitive(
    const std::string & value,
    size_t offset,
    const char * prefix
) {
    for (size_t ii = 0; prefix[ii] != '\0'; ii++) {
        if (offset + ii >= value.size()) return false;
        const auto actual = static_cast<unsigned char>(value[offset + ii]);
        const auto expected = static_cast<unsigned char>(prefix[ii]);
        if (std::tolower(actual) != std::tolower(expected)) return false;
    }
    return true;
}

inline bool isBareHTMLDocument(const std::string & value) {
    const size_t offset = firstContentOffset(value);
    return startsWithCaseInsensitive(value, offset, "<!doctype html") ||
           startsWithCaseInsensitive(value, offset, "<html") ||
           startsWithCaseInsensitive(value, offset, "<head") ||
           startsWithCaseInsensitive(value, offset, "<body");
}

inline std::string lowerCopy(std::string value) {
    for (char & ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

inline bool endsWith(const std::string & value, const std::string & suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline size_t headerSeparator(const std::string & value, size_t & separatorLength) {
    size_t at = value.find("\r\n\r\n");
    if (at != std::string::npos) {
        separatorLength = 4;
        return at;
    }
    at = value.find("\n\n");
    separatorLength = at == std::string::npos ? 0 : 2;
    return at;
}

inline bool multipartBoundaryAt(
    const std::string & value,
    size_t bodyOffset,
    std::string & boundary
) {
    size_t lineStart = bodyOffset;
    while (lineStart < value.size()) {
        size_t lineEnd = value.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = value.size();
        size_t trimmedEnd = lineEnd;
        while (trimmedEnd > lineStart &&
               (value[trimmedEnd - 1] == '\r' || value[trimmedEnd - 1] == ' ' || value[trimmedEnd - 1] == '\t')) {
            trimmedEnd--;
        }
        if (trimmedEnd > lineStart) {
            if (trimmedEnd - lineStart < 4 || value.compare(lineStart, 2, "--") != 0) return false;
            std::string candidate = value.substr(lineStart + 2, trimmedEnd - lineStart - 2);
            if (endsWith(candidate, "--")) return false;
            if (candidate.find_first_of("\r\n") != std::string::npos) return false;
            const std::string closing = "--" + candidate + "--";
            if (value.find(closing, trimmedEnd) == std::string::npos) return false;
            boundary = candidate;
            return true;
        }
        lineStart = lineEnd == value.size() ? value.size() : lineEnd + 1;
    }
    return false;
}

inline std::string repairMultipartEnvelope(const std::string & value) {
    size_t separatorLength = 0;
    const size_t contentOffset = firstContentOffset(value);
    const bool startsWithBoundary = value.compare(contentOffset, 2, "--") == 0;
    size_t separator = startsWithBoundary ? std::string::npos
                                          : headerSeparator(value, separatorLength);
    size_t bodyOffset = separator == std::string::npos ? firstContentOffset(value)
                                                       : separator + separatorLength;
    std::string boundary;
    if (!multipartBoundaryAt(value, bodyOffset, boundary)) return value;

    const std::string contentType =
        "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"";
    if (separator == std::string::npos) {
        return "MIME-Version: 1.0\r\n" + contentType + "\r\n\r\n" + value;
    }

    const std::string headers = value.substr(0, separator);
    const std::string lowerHeaders = lowerCopy(headers);
    const size_t contentTypeAt = lowerHeaders.find("content-type:");
    if (contentTypeAt == std::string::npos) {
        return headers + "\r\n" + contentType + "\r\n\r\n" + value.substr(bodyOffset);
    }

    // A few SmarterMail builds return the multipart body with an incorrect
    // top-level boundary parameter. Replace only the top-level Content-Type
    // field (including folded continuation lines); the MIME parts themselves
    // remain byte-for-byte intact.
    size_t fieldEnd = headers.find('\n', contentTypeAt);
    if (fieldEnd == std::string::npos) fieldEnd = headers.size();
    while (fieldEnd < headers.size()) {
        size_t next = fieldEnd + 1;
        if (next >= headers.size() || (headers[next] != ' ' && headers[next] != '\t')) break;
        fieldEnd = headers.find('\n', next);
        if (fieldEnd == std::string::npos) {
            fieldEnd = headers.size();
            break;
        }
    }
    std::string field = headers.substr(contentTypeAt, fieldEnd - contentTypeAt);
    std::string lowerField = lowerCopy(field);
    if (lowerField.find("multipart/") != std::string::npos) {
        const size_t boundaryAt = lowerField.find("boundary=");
        if (boundaryAt != std::string::npos) {
            size_t valueStart = boundaryAt + 9;
            while (valueStart < field.size() &&
                   (field[valueStart] == ' ' || field[valueStart] == '\t')) valueStart++;
            size_t valueEnd = valueStart;
            if (valueStart < field.size() && field[valueStart] == '"') {
                valueStart++;
                valueEnd = field.find('"', valueStart);
            } else {
                while (valueEnd < field.size() && field[valueEnd] != ';' &&
                       field[valueEnd] != '\r' && field[valueEnd] != '\n' &&
                       field[valueEnd] != ' ' && field[valueEnd] != '\t') valueEnd++;
            }
            if (valueEnd != std::string::npos &&
                field.substr(valueStart, valueEnd - valueStart) == boundary) return value;
            if (valueEnd != std::string::npos) {
                field.replace(valueStart, valueEnd - valueStart, boundary);
            }
        } else {
            while (!field.empty() && (field.back() == '\r' || field.back() == '\n')) field.pop_back();
            field += "; boundary=\"" + boundary + "\"";
        }
    } else {
        field = contentType;
    }
    std::string repaired = headers.substr(0, contentTypeAt) + field;
    if (fieldEnd < headers.size()) repaired += headers.substr(fieldEnd);
    return repaired + "\r\n\r\n" + value.substr(bodyOffset);
}

inline std::string normalize(const std::string & value) {
    if (!isBareHTMLDocument(value)) return repairMultipartEnvelope(value);

    return "MIME-Version: 1.0\r\n"
           "Content-Type: text/html; charset=utf-8\r\n"
           "Content-Transfer-Encoding: 8bit\r\n"
           "\r\n" + value;
}

} // namespace SmarterMailRawMessage

#endif
