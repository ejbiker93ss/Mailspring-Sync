#ifndef SmarterMailDataWorker_hpp
#define SmarterMailDataWorker_hpp

#include <memory>

#include "Account.hpp"
#include "Contact.hpp"
#include "Event.hpp"

class SmarterMailDataWorker {
public:
    explicit SmarterMailDataWorker(std::shared_ptr<Account> account);
    ~SmarterMailDataWorker();

    void run();
    void runContacts();
    void runCalendars();
    void writeAndResyncContact(std::shared_ptr<Contact> contact);
    void deleteContact(std::shared_ptr<Contact> contact);
    void rebuildContactGroup(std::shared_ptr<Contact> contact);
    void writeAndResyncEvent(std::shared_ptr<Event> event);
    void deleteEvent(std::shared_ptr<Event> event);

private:
    std::shared_ptr<Account> account;
    class MailStore * store;
};

#endif
