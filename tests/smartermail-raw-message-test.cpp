#include "../MailSync/SmarterMailRawMessage.hpp"

#include <cassert>
#include <string>

int main() {
    using SmarterMailRawMessage::normalize;

    assert(SmarterMailRawMessage::isClosingBoundaryOnly(
        "_006_SA3PR14MB70263191E524297F7A46DFA4C2B8SA3PR14MB7026namp_--"));
    assert(SmarterMailRawMessage::isClosingBoundaryOnly("--==_mimepart_example--\r\n"));
    assert(SmarterMailRawMessage::isClosingBoundaryOnly("=-xGT7zo0wYKfbQCXRPNNBFQ==--"));
    assert(!SmarterMailRawMessage::isClosingBoundaryOnly("Thanks --"));
    assert(!SmarterMailRawMessage::isClosingBoundaryOnly(
        "<html><body>_005_BYAPR17MB2456namp_--</body></html>"));

    const std::string structured = SmarterMailRawMessage::structuredBodyMime(
        "<html><body>Fast body</body></html>", "Fast body");
    assert(structured.find("Content-Type: text/html; charset=utf-8") != std::string::npos);
    assert(structured.find("Fast body") != std::string::npos);

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

    const std::string blockedHTML =
        "<html><img width=\"473\" src=\"https://mail.example.com/interface/img/s.png\"></html>";
    assert(SmarterMailRawMessage::hasBlockedRemoteImages(blockedHTML));
    assert(SmarterMailRawMessage::hasBlockedRemoteImages(
        "<IMG SRC='/interface/img/s.png?x=1'>"));
    assert(!SmarterMailRawMessage::hasBlockedRemoteImages(
        "<a href='/interface/img/s.png'>link</a>"));
    assert(!SmarterMailRawMessage::hasBlockedRemoteImages(
        "<img src='https://apple.example/logo.png'>"));
    const std::string originalHTML = "<html><img src='https://apple.example/logo.png'></html>";
    const std::string originalMIME = SmarterMailRawMessage::structuredBodyMime(originalHTML, "");
    assert(SmarterMailRawMessage::displayMime(blockedHTML, "Dear Emory", originalMIME) == originalMIME);
    assert(SmarterMailRawMessage::displayMime(blockedHTML, "Dear Emory", originalHTML) == originalMIME);
    assert(SmarterMailRawMessage::displayMime(blockedHTML, "Dear Emory", validMultipart) == validMultipart);
    for (const auto & invalid : {std::string("success"), std::string("--==_mimepart_example--"),
                                SmarterMailRawMessage::structuredBodyMime(blockedHTML, "")}) {
        assert(SmarterMailRawMessage::displayMime(blockedHTML, "Dear Emory", invalid) ==
            SmarterMailRawMessage::structuredBodyMime(blockedHTML, "Dear Emory"));
    }
    // Do not replace healthy structured HTML just because raw was supplied.
    assert(SmarterMailRawMessage::displayMime(originalHTML, "", validMultipart) == originalMIME);

    const std::string entityEncodedDocument =
        "&lt;!DOCTYPE html&gt;&lt;html&gt;&lt;body&gt;"
        "<p title=&quot;A &amp; B&quot;&gt;Meet Passion Fruit &amp; enjoy &#x1F334;"
        "&lt;/p&gt;&lt;/body&gt;&lt;/html&gt;";
    const std::string decodedDocument = SmarterMailRawMessage::decodeEscapedHTMLDocument(entityEncodedDocument);
    assert(decodedDocument.find("<!DOCTYPE html><html><body>") == 0);
    assert(decodedDocument.find("title=\"A & B\">") != std::string::npos);
    assert(decodedDocument.find("Meet Passion Fruit & enjoy \xF0\x9F\x8C\xB4") != std::string::npos);
    assert(SmarterMailRawMessage::structuredBodyMime(entityEncodedDocument, "") ==
        SmarterMailRawMessage::structuredBodyMime(decodedDocument, ""));
    const std::string ordinaryText = "The text &lt;html&gt; is a literal example.";
    assert(SmarterMailRawMessage::decodeEscapedHTMLDocument(ordinaryText) == ordinaryText);

    return 0;
}
