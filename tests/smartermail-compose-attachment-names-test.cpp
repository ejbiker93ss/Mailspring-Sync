#include "../MailSync/SmarterMailCompose.hpp"

#include <cassert>

int main() {
    using namespace SmarterMailCompose;

    const std::set<std::string> originals = {"image.png", "audit letter.pdf"};
    std::set<std::string> staged;
    assert(uniqueAttachmentName("image.png", originals, staged) == "image.png");
    assert(uniqueAttachmentName("image.png", originals, staged) == "image (2).png");
    assert(uniqueAttachmentName("image.png", originals, staged) == "image (3).png");
    assert(uniqueAttachmentName("image.png", originals, staged) == "image (4).png");
    assert(uniqueAttachmentName("Audit Letter.pdf", originals, staged) == "Audit Letter.pdf");

    const std::set<std::string> reserved = {"image.png", "image (2).png", "readme"};
    staged.clear();
    assert(uniqueAttachmentName("image.png", reserved, staged) == "image.png");
    assert(uniqueAttachmentName("IMAGE.PNG", reserved, staged) == "IMAGE (3).PNG");
    assert(uniqueAttachmentName("image (2).png", reserved, staged) == "image (2).png");
    assert(uniqueAttachmentName("readme", reserved, staged) == "readme");
    assert(uniqueAttachmentName("readme", reserved, staged) == "readme (2)");
}
