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

    const std::string headerlessMultipart =
        "--==_mimepart_example\r\n"
        "Content-Type: text/html; charset=utf-8\r\n\r\n"
        "<html><body>Hello</body></html>\r\n"
        "--==_mimepart_example--\r\n";
    const std::string repairedHeaderless = normalize(headerlessMultipart);
    assert(repairedHeaderless.find(
        "Content-Type: multipart/mixed; boundary=\"==_mimepart_example\"") != std::string::npos);
    assert(repairedHeaderless.substr(repairedHeaderless.size() - headerlessMultipart.size()) == headerlessMultipart);

    const std::string missingContentType =
        "From: Support <support@example.com>\r\n"
        "Subject: Example\r\n\r\n"
        "--==_mimepart_example\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        "Hello\r\n"
        "--==_mimepart_example--\r\n";
    assert(normalize(missingContentType).find(
        "Content-Type: multipart/mixed; boundary=\"==_mimepart_example\"") != std::string::npos);

    const std::string wrongBoundary =
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/alternative; boundary=\"wrong\"\r\n\r\n"
        "--==_mimepart_example\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        "Hello\r\n"
        "--==_mimepart_example--\r\n";
    const std::string repairedBoundary = normalize(wrongBoundary);
    assert(repairedBoundary.find("boundary=\"wrong\"") == std::string::npos);
    assert(repairedBoundary.find("boundary=\"==_mimepart_example\"") != std::string::npos);

    const std::string validMultipart =
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/alternative; boundary=\"==_mimepart_example\"\r\n\r\n"
        "--==_mimepart_example\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        "Hello\r\n"
        "--==_mimepart_example--\r\n";
    assert(normalize(validMultipart) == validMultipart);

    return 0;
}
