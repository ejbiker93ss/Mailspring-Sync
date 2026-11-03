// Verifies MailCore reassembles RFC 2231 attachment names (filename*= and
// numbered filename*0*= / name*0*= pieces) that libetpan leaves unparsed.
#include <MailCore/MailCore.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using namespace mailcore;

static std::string nameFor(const std::string & partHeaders)
{
    const std::string mime =
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"b\"\r\n\r\n"
        "--b\r\nContent-Type: text/plain\r\n\r\nhello\r\n"
        "--b\r\n" + partHeaders +
        "Content-Transfer-Encoding: base64\r\n\r\nUEsDBA==\r\n--b--\r\n";
    AutoreleasePool pool;
    MessageParser * parser = MessageParser::messageParserWithData(
        Data::dataWithBytes(mime.data(), (unsigned int) mime.size()));
    Array * attachments = parser->attachments();
    assert(attachments->count() == 1);
    String * filename = ((Attachment *) attachments->objectAtIndex(0))->filename();
    return filename ? filename->UTF8Characters() : "";
}

int main()
{
    const std::string expected =
        "hours by day with shifts _ Bonus Calculation Information_11_02_2026_11_49_38_AM.xlsx";

    // Numbered, percent-encoded pieces in Content-Disposition (Outlook / Exchange).
    assert(nameFor(
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment;\r\n"
        " filename*0*=utf-8''hours%20by%20day%20with%20shifts%20_%20Bonus%20Calculation%20;\r\n"
        " filename*1*=Information_11_02_2026_11_49_38_AM.xlsx\r\n") == expected);

    // Only the Content-Type name carries the RFC 2231 value.
    assert(nameFor(
        "Content-Type: application/octet-stream;\r\n"
        " name*0*=utf-8''hours%20by%20day%20with%20shifts%20_%20Bonus%20Calculation%20;\r\n"
        " name*1*=Information_11_02_2026_11_49_38_AM.xlsx\r\n"
        "Content-Disposition: attachment\r\n") == expected);

    // Single extended value with a non-ASCII character.
    assert(nameFor(
        "Content-Type: application/pdf\r\n"
        "Content-Disposition: attachment; filename*=utf-8''r%C3%A9sum%C3%A9.pdf\r\n") ==
        "r\xC3\xA9sum\xC3\xA9.pdf");

    // Numbered pieces without encoding.
    assert(nameFor(
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename*0=\"report \"; filename*1=\"2026.xlsx\"\r\n") ==
        "report 2026.xlsx");

    // Plain names still win and are unchanged.
    assert(nameFor(
        "Content-Type: application/octet-stream; name=\"ignored.bin\"\r\n"
        "Content-Disposition: attachment; filename=\"plain.xlsx\"\r\n") == "plain.xlsx");

    // A part with no name at all stays unnamed.
    assert(nameFor("Content-Type: application/octet-stream\r\nContent-Disposition: attachment\r\n").empty());

    std::cout << "PASS: RFC 2231 filenames (numbered, Content-Type, extended UTF-8, unencoded), plain names, unnamed parts\n";
    return 0;
}
