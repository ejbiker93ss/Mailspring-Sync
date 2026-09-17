#include "../MailSync/SmarterMailRawMessage.hpp"

#include <cassert>
#include <string>

int main() {
    using SmarterMailRawMessage::normalize;

    const std::string html = "<!doctype html><html><body>Hello</body></html>";
    const std::string normalized = normalize(html);
    assert(normalized.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos);
    assert(normalized.substr(normalized.size() - html.size()) == html);

    const std::string spacedHTML = " \r\n<HTML><body>Hello</body></HTML>";
    assert(normalize(spacedHTML).find("Content-Type: text/html") != std::string::npos);

    const std::string mime =
        "Content-Type: text/html; charset=utf-8\r\n\r\n<html><body>Hello</body></html>";
    assert(normalize(mime) == mime);

    const std::string plaintext = "Use <html> as an example.";
    assert(normalize(plaintext) == plaintext);

    return 0;
}
