//
//  Folder.cpp
//  MailSync
//
//  Created by Ben Gotow on 6/17/17.
//  Copyright © 2017 Foundry 376. All rights reserved.
//
//  Use of this file is subject to the terms and conditions defined
//  in 'LICENSE.md', which is part of the SummerMail-Sync package.
//

#include "TaskProcessor.hpp"
#include "MailProcessor.hpp"
#include "MailStoreTransaction.hpp"
#include "MailUtils.hpp"
#include "Thread.hpp"
#include "Message.hpp"
#include "MailUtils.hpp"
#include "DAVWorker.hpp"
#include "DAVUtils.hpp"
#include "GoogleContactsWorker.hpp"
#include "File.hpp"
#include "ContactGroup.hpp"
#include "Event.hpp"
#include "icalendar.h"
#include "constants.h"
#include "ProgressCollectors.hpp"
#include "SyncException.hpp"
#include "NetworkRequestUtils.hpp"
#include "XOAuth2TokenManager.hpp"
#include "SmarterMailClient.hpp"
#include "ReplyHeaders.hpp"
#include "SmarterMailCompose.hpp"
#include "MoveResult.hpp"

#include <sstream>
#include <algorithm>
#include <iomanip>
#include <thread>
#include <chrono>
#include <set>

#if defined(_MSC_VER)
#include <direct.h>
#include <codecvt>
#include <locale>
#include <sys/utime.h>
#else
#include <sys/time.h>
#endif

using namespace std;

static string taskGraphUrlEncode(const string & value) {
    CURL *curl = curl_easy_init();
    char *encoded = curl_easy_escape(curl, value.c_str(), (int)value.size());
    string result = encoded ? encoded : "";
    if (encoded) curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

static Data * fetchMicrosoftGraphMIME(shared_ptr<Account> account, Message * message) {
    if (message->graphId().empty()) {
        throw SyncException("not-found", "Microsoft Graph message ID is unavailable.", false);
    }
    auto token = SharedXOAuth2TokenManager()->partsForAccount(account).accessToken;
    string url = MicrosoftGraphBaseURL(account) + "/messages/" +
        taskGraphUrlEncode(message->graphId()) + "/$value";
    CURL * request = CreateMicrosoftGraphRequest(url, "GET", token);
    string raw = PerformRequest(request);
    CleanupCurlRequest(request);
    return Data::dataWithBytes(raw.c_str(), (unsigned int)raw.size());
}
using namespace mailcore;
using namespace nlohmann;

static void setFileModificationTime(const string & filepath, time_t timestamp) {
#ifdef _MSC_VER
    wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
    struct _utimbuf times;
    times.actime = timestamp;
    times.modtime = timestamp;
    _wutime(convert.from_bytes(filepath).c_str(), &times);
#else
    struct timeval times[2];
    times[0].tv_sec = timestamp;
    times[0].tv_usec = 0;
    times[1].tv_sec = timestamp;
    times[1].tv_usec = 0;
    utimes(filepath.c_str(), times);
#endif
}

// A helper function that can move messages between folders and update the provided
// messages remoteUIDs, even if UIDPLUS and/or MOVE extensions are not present.

void _moveMessagesResilient(IMAPSession * session, String * path, Folder * destFolder, IndexSet * uids, vector<shared_ptr<Message>> messages) {
    ErrorCode err = ErrorCode::ErrorNone;
    HashMap * uidmap = nullptr;
    String * destPath = AS_MCSTR(destFolder->path());
    bool mustApplyAttributes = false;
    
    // First, perform the action - either the MOVE or the COPY, STORE, EXPUNGE
    // if IMAPCapabilityMove is not present.
    if (session->storedCapabilities()->containsIndex(IMAPCapabilityMove)) {
        session->moveMessages(path, uids, destPath, &uidmap, &err);
        if (err != ErrorCode::ErrorNone) {
            throw SyncException(err, "moveMessages");
        }
    } else {
        session->copyMessages(path, uids, destPath, &uidmap, &err);
        if (err != ErrorCode::ErrorNone) {
            throw SyncException(err, "moveMessages(copy)");
        }
        session->storeFlagsByUID(path, uids, IMAPStoreFlagsRequestKindAdd, MessageFlagDeleted, &err);
        if (err != ErrorNone) throw SyncException(err, "moveMessages(mark deleted after copy)");
        if (session->storedCapabilities()->containsIndex(IMAPCapabilityUIDPlus)) {
            session->expungeUIDs(path, uids, &err);
        } else {
            session->expunge(path, &err);
        }
        if (err != ErrorCode::ErrorNone) {
            throw SyncException(err, "moveMessages(copy cleanup)");
        }
        mustApplyAttributes = true;
    }

    // An OK response alone is not proof that every requested UID moved. Only
    // fetch flags for this batch, never scan the source mailbox or fetch bodies.
    Array * remaining = session->fetchMessagesByUID(path, IMAPMessagesRequestKindFlags, uids, nullptr, &err);
    if (err != ErrorNone) throw SyncException(err, "moveMessages(verify source)");
    if (!remaining) throw SyncException("move-unconfirmed", "Source verification unavailable", false);
    set<uint32_t> remainingUIDs;
    for (unsigned int i = 0; i < remaining->count(); ++i) {
        remainingUIDs.insert(((IMAPMessage *)remaining->objectAtIndex(i))->uid());
    }
    bool missingMapping = false;

    // Only returned if UIDPLUS extension is present and the server tells us
    // which UIDs in the old folder map to which UIDs in the new folder.
    if (uidmap != nullptr) {
        for (auto msg : messages) {
            if (remainingUIDs.count(msg->remoteUID())) continue;
            Value * currentUID = Value::valueWithUnsignedLongValue(msg->remoteUID());
            Value * newUID = (Value *)uidmap->objectForKey(currentUID);
            if (!newUID) {
                missingMapping = true;
                continue;
            }
            msg->setRemoteFolder(destFolder);
            msg->setRemoteUID(newUID->unsignedIntValue());
        }
    } else {
        // UIDPLUS is not supported, we need to manually find the messages. Thankfully moves
        // should add higher UIDs to the folder so we can grab the last few and get the messages
        auto status = session->folderStatus(destPath, &err);
        if (err != ErrorNone) throw SyncException(err, "moveMessages(destination status)");
        if (!status) throw SyncException("move-unconfirmed", "Destination status unavailable", false);
        IMAPMessagesRequestKind kind = MailUtils::messagesRequestKindFor(session->storedCapabilities(), true);
        
        if (status != nullptr) {
            // Calculate a safe lower bound to avoid underflow with unsigned arithmetic.
            // We search from (uidNext - messages.size() * 2) to find the moved messages,
            // using a multiplier of 2 to account for potential gaps in UID assignment.
            uint32_t uidNext = status->uidNext();
            uint32_t searchRange = (uint32_t)messages.size() * 2;
            uint32_t min = (uidNext > searchRange) ? (uidNext - searchRange) : 1;
            if (uidNext <= min) throw SyncException("move-unconfirmed", "Destination UID window unavailable", false);
            IndexSet * set = IndexSet::indexSetWithRange(RangeMake(min, uidNext - min));
            Array * movedMessages = session->fetchMessagesByUID(destPath, kind, set, nullptr, &err);
            if (err != ErrorNone) throw SyncException(err, "moveMessages(destination lookup)");
            if (!movedMessages) throw SyncException("move-unconfirmed", "Destination lookup unavailable", false);
            for (auto msg : messages) {
                if (remainingUIDs.count(msg->remoteUID())) continue;
                bool found = false;
                for (unsigned int ii = 0; ii < movedMessages->count(); ii ++) {
                    IMAPMessage * movedMessage = (IMAPMessage*)movedMessages->objectAtIndex(ii);
                    string movedId = MailUtils::idForMessage(msg->accountId(), destFolder->path(), movedMessage);
                    if (msg->id() == movedId || msg->_data.value("physicalBaseId", string()) == movedId) {
                        msg->setRemoteFolder(destFolder);
                        msg->setRemoteUID(movedMessage->uid());
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    missingMapping = true;
                }
            }
        }
    }
    
    if (!remainingUIDs.empty() || missingMapping) {
        throw SyncException("move-incomplete", "Some messages could not be confirmed in the destination. Check both folders before retrying.", false);
    }
    if (mustApplyAttributes) {
        for (auto msg : messages) {
            if (msg->remoteFolderId() == destFolder->id()) {
                MessageFlag flags = MessageFlagNone;
                if (msg->isStarred())
                    flags = (MessageFlag)(flags | MessageFlagFlagged);
                if (!msg->isUnread())
                    flags = (MessageFlag)(flags | MessageFlagSeen);
                if (msg->isDraft())
                    flags = (MessageFlag)(flags | MessageFlagDraft);
        
                if (flags != MessageFlagNone) {
                    session->storeFlagsByUID(destPath, IndexSet::indexSetWithIndex(msg->remoteUID()), IMAPStoreFlagsRequestKindSet, flags, &err);
                    if (err != ErrorNone) throw SyncException(err, "moveMessages(destination flags)");
                }
            }
        }
    }
}

// A helper function to permanently remove messages by UID from a given folder path. When a trash folder
// and CapabilityMove are present, it moves there and expunges. Otherwises it expunges in place.

void _removeMessagesResilient(IMAPSession * session, MailStore * store, string accountId, String * path, IndexSet * uids) {
    ErrorCode err = ErrorCode::ErrorNone;

    // First, add the "DELETED" flag to the given messages
    session->storeFlagsByUID(path, uids, IMAPStoreFlagsRequestKindAdd, MessageFlagDeleted, &err);
    if (err != ErrorNone) {
        spdlog::get("logger")->info("X- removeMessages could not add deleted flag (error: {})", ErrorCodeToTypeMap[err]);
        throw SyncException(err, "removeMessages(mark deleted)");
    }
    
    // If possible, move the messages to the identified trash folder.
    // Sometimes [on Gmail] this is necessary to properly mark them as deleted.
    String * trashPath = nullptr;
    if (session->storedCapabilities()->containsIndex(IMAPCapabilityMove)) {
        auto trash = store->find<Folder>(Query().equal("accountId", accountId).equal("role", "trash"));
        if (trash != nullptr) trashPath = AS_MCSTR(trash->path());
    }

    if (trashPath != nullptr) {
        HashMap * uidMapping = nullptr;
        session->moveMessages(path, uids, trashPath, &uidMapping, &err);
        if (err != ErrorNone) {
            spdlog::get("logger")->info("X- removeMessages could not move to {} (error: {})", trashPath->UTF8Characters(), ErrorCodeToTypeMap[err]);
        } else {
            // If we were successful moving to the trash, we will now expunge from here, and the UIDs
            // we had before are no longer valid so we'll need to expunge the entire folder.
            uids->removeAllIndexes();
            path = trashPath;
            
            // If we got a UID mapping back, we can make an Expunge UIDs request for the specific deleted UIDs.
            // We also re-flag them as deleted because Gmail removes the Deleted attribute when the items are moved.
            if (uidMapping) {
                Array * uidsInNewFolder = uidMapping->allValues();
                for (unsigned int ii = 0; ii < uidsInNewFolder->count(); ii ++) {
                    Value * val = (Value *)uidsInNewFolder->objectAtIndex(ii);
                    uids->addIndex(val->unsignedLongValue());
                }
                spdlog::get("logger")->info("-- removeMessages re-applying deleted flag after moving to {}", trashPath->UTF8Characters());
                session->storeFlagsByUID(path, uids, IMAPStoreFlagsRequestKindAdd, MessageFlagDeleted, &err);
                if (err != ErrorNone) {
                    spdlog::get("logger")->info("X- removeMessages could not add deleted flag (error: {})", ErrorCodeToTypeMap[err]);
                    throw SyncException(err, "removeMessages(mark deleted in trash)");
                }
            }
        }
    }
    
    if (uids->count() > 0) {
        spdlog::get("logger")->info("-- removeMessages Expunging (UIDs) from {}", path->UTF8Characters());
        session->expungeUIDs(path, uids, &err);
        if (err != ErrorNone) {
            spdlog::get("logger")->info("-- removeMessages Expunge (UIDs) failed (error: {})", ErrorCodeToTypeMap[err]);
            spdlog::get("logger")->info("-- removeMessages Expunging (Basic) from {}", path->UTF8Characters());
            err = ErrorNone;
            session->expunge(path, &err);
        }
    } else {
        spdlog::get("logger")->info("-- removeMessages Expunging (Basic) from {}", path->UTF8Characters());
        session->expunge(path, &err);
    }

    if (err != ErrorNone) {
        spdlog::get("logger")->info("X- removeMessages Expunge failed (error: {})", ErrorCodeToTypeMap[err]);
        throw SyncException(err, "removeMessages(expunge)");
    }
    if (uids->count() > 0) {
        Array * remaining = session->fetchMessagesByUID(path, IMAPMessagesRequestKindFlags, uids, nullptr, &err);
        if (err != ErrorNone) throw SyncException(err, "removeMessages(verify)");
        if (!remaining || remaining->count() > 0) {
            throw SyncException("delete-unconfirmed", "The server did not confirm removal of every requested message.", false);
        }
    }
}


// Small functions that we pass to the generic ChangeMessages runner

void _applyUnread(Message * msg, json & data) {
    msg->setUnread(data["unread"].get<bool>());
}

void _applyUnreadInIMAPFolder(IMAPSession * session, String * path, IndexSet * uids, vector<shared_ptr<Message>> messages, json & data) {
    ErrorCode err = ErrorCode::ErrorNone;
    if (data["unread"].get<bool>() == false) {
        session->storeFlagsByUID(path, uids, IMAPStoreFlagsRequestKindAdd, MessageFlagSeen, &err);
    } else {
        session->storeFlagsByUID(path, uids, IMAPStoreFlagsRequestKindRemove, MessageFlagSeen, &err);
    }
    if (err != ErrorCode::ErrorNone) {
        throw SyncException(err, "storeFlagsByUID");
    }
}

void _applyStarred(Message * msg, json & data) {
    msg->setStarred(data["starred"].get<bool>());
}

void _applyStarredInIMAPFolder(IMAPSession * session, String * path, IndexSet * uids, vector<shared_ptr<Message>> messages, json & data) {
    ErrorCode err = ErrorCode::ErrorNone;
    if (data["starred"].get<bool>() == true) {
        session->storeFlagsByUID(path, uids, IMAPStoreFlagsRequestKindAdd, MessageFlagFlagged, &err);
    } else {
        session->storeFlagsByUID(path, uids, IMAPStoreFlagsRequestKindRemove, MessageFlagFlagged, &err);
    }
    if (err != ErrorCode::ErrorNone) {
        throw SyncException(err, "storeFlagsByUID");
    }
}

void _applyFolder(Message * msg, json & data) {
    Folder folder{data["folder"]};
    msg->setClientFolder(&folder);
}

void _applyFolderMoveInIMAPFolder(IMAPSession * session, String * path, IndexSet * uids, vector<shared_ptr<Message>> messages, json & data) {
    Folder destFolder{data["folder"]};
    
    _moveMessagesResilient(session, path, &destFolder, uids, messages);
}

string _xgmKeyForLabel(json & label) {
    string role = label["role"].get<string>();
    string path = label["path"].get<string>();
    if (role == "inbox") {
        return "\\Inbox";
    }
    if (role == "important") {
        return "\\Important";
    }
    return path;
}

void _applyLabels(Message * msg, json & data) {
    json & toAdd = data["labelsToAdd"];
    json & toRemove = data["labelsToRemove"];
    json & labels = msg->remoteXGMLabels();
    
    for (auto & item : toAdd) {
        string xgmValue = _xgmKeyForLabel(item);
        bool found = false;
        for (auto & existing : labels) {
            if (existing.get<string>() == xgmValue) {
                found = true;
                break;
            }
        }
        if (found == false) {
            labels.push_back(xgmValue);
        }
    }
    for (auto & item : toRemove) {
        string xgmValue = _xgmKeyForLabel(item);
        for (int i = (int)labels.size() - 1; i >= 0; i --) {
            if (labels.at(i).get<string>() == xgmValue) {
                labels.erase(i);
            }
        }
    }
    msg->setRemoteXGMLabels(labels);
}

void _applyLabelChangeInIMAPFolder(IMAPSession * session, String * path, IndexSet * uids, vector<shared_ptr<Message>> messages, json & data) {
    AutoreleasePool pool;

    ErrorCode err = ErrorCode::ErrorNone;
    Array * toAdd = new mailcore::Array{};
    toAdd->autorelease();
    for (auto & item : data["labelsToAdd"]) {
        toAdd->addObject(AS_MCSTR(_xgmKeyForLabel(item)));
    }

    Array * toRemove = new mailcore::Array{};
    toRemove->autorelease();
    for (auto & item : data["labelsToRemove"]) {
        toRemove->addObject(AS_MCSTR(_xgmKeyForLabel(item)));
    }
    
    if (toAdd->count() > 0) {
        session->storeLabelsByUID(path, uids, IMAPStoreFlagsRequestKindAdd, toAdd, &err);
        if (err != ErrorCode::ErrorNone) {
            throw SyncException(err, "storeLabelsByUID - add");
        }
    }
    if (toRemove->count() > 0) {
        session->storeLabelsByUID(path, uids, IMAPStoreFlagsRequestKindRemove, toRemove, &err);
        if (err != ErrorCode::ErrorNone) {
            throw SyncException(err, "storeLabelsByUID - remove");
        }
    }
}


TaskProcessor::TaskProcessor(shared_ptr<Account> account, MailStore * store, IMAPSession * session) :
    account(account),
    store(store),
    logger(spdlog::get("logger")),
    session(session) {
}

void TaskProcessor::cleanupTasksAfterLaunch() {
    // look for tasks that are in the `local` state. The app most likely crashed while running these
    // tasks, since they're saved immediately before performLocal is run. Delete them to avoid
    // the app crashing again.
    auto stuck = store->findAll<Task>(Query().equal("accountId", account->id()).equal("status", "local"));
    for (auto & t : stuck) {
        store->remove(t.get());
    }

    cleanupOldTasksAtRuntime();
}

void TaskProcessor::cleanupOldTasksAtRuntime() {
    // Keep only the last 100 completed / cancelled tasks
    //
    // Note: We need to load and then delete these rather than using a DELETE query
    // because the app observes the entire Task queue using a QuerySubscription, and
    // if we delete them from under it, it won't release them from the array.
    //
    SQLite::Statement count(store->db(), "SELECT COUNT(id) FROM Task WHERE accountId = ? AND (status = \"complete\" OR status = \"cancelled\")");
    count.bind(1, account->id());
    count.executeStep();
    int countToRemove = count.getColumn(0).getInt() - 100;
    count.reset();

    if (countToRemove > 10) { // slop
        MailStoreTransaction transaction{store, "cleanupOldTasksAtRuntime"};

        SQLite::Statement unneeded(store->db(), "SELECT data FROM Task WHERE accountId = ? AND (status = \"complete\" OR status = \"cancelled\") ORDER BY rowid ASC LIMIT ?");
        unneeded.bind(1, account->id());
        unneeded.bind(2, countToRemove);
        vector<shared_ptr<Task>> unneededTasks{};
        while (unneeded.executeStep()) {
            unneededTasks.push_back(make_shared<Task>(unneeded));
        }
        unneeded.reset();
        for (auto & t : unneededTasks) {
            store->remove(t.get());
        }
        
        transaction.commit();
    }
}

// PerformLocal is run from the main thread as tasks are received from the client

void TaskProcessor::performLocal(Task * task) {
    string cname = task->constructorName();
    
    logger->info("[{}] Running {} performLocal:", task->id(), cname);

    try {
        store->save(task);
    } catch (SQLite::Exception & ex) {
        logger->error("[{}] -- Exception: Task could not be saved to the database. {}", task->id(), ex.what());
        return;
    }

    try {
        if (task->accountId() != account->id()) {
            throw SyncException("generic", "You must provide an account id.", false);
        }

        if (cname == "ChangeUnreadTask") {
            performLocalChangeOnMessages(task, _applyUnread);
            
        } else if (cname == "ChangeStarredTask") {
            performLocalChangeOnMessages(task, _applyStarred);

        } else if (cname == "ChangeFolderTask") {
            performLocalChangeOnMessages(task, _applyFolder);
            
        } else if (cname == "ChangeLabelsTask") {
            performLocalChangeOnMessages(task, _applyLabels);
        
        } else if (cname == "SyncbackDraftTask") {
            performLocalSaveDraft(task);
            
        } else if (cname == "DestroyDraftTask") {
            performLocalDestroyDraft(task);
            
        } else if (cname == "SyncbackCategoryTask") {
            performLocalSyncbackCategory(task);
            
        } else if (cname == "DestroyCategoryTask") {
            // nothing

        } else if (cname == "SendDraftTask") {
            // nothing

        } else if (cname == "SyncbackMetadataTask") {
            performLocalSyncbackMetadata(task);
            
        } else if (cname == "SendFeatureUsageEventTask") {
            // nothing
        
        } else if (cname == "ChangeRoleMappingTask") {
            performLocalChangeRoleMapping(task);
        
        } else if (cname == "ExpungeAllInFolderTask") {
            // nothing

        } else if (cname == "GetMessageRFC2822Task") {
            // nothing

        } else if (cname == "GetManyRFC2822Task") {
            // nothing — all work happens in performRemote

        } else if (cname == "CrossAccountMoveFolderTask") {
            // Both phases perform network I/O in performRemote.

        } else if (cname == "EventRSVPTask") {
            // nothing

        } else if (cname == "DestroyContactTask") {
            performLocalDestroyContact(task);

        } else if (cname == "SyncbackContactTask") {
            performLocalSyncbackContact(task);

        } else if (cname == "ChangeContactGroupMembershipTask") {
            performLocalChangeContactGroupMembership(task);

        } else if (cname == "SyncbackContactGroupTask") {
            performLocalSyncbackContactGroup(task);

        } else if (cname == "DestroyContactGroupTask") {
            performLocalDestroyContactGroup(task);

        } else if (cname == "SyncbackEventTask") {
            performLocalSyncbackEvent(task);

        } else if (cname == "DestroyEventTask") {
            performLocalDestroyEvent(task);

        } else {
            logger->error("Unsure of how to process this task type {}", cname);
        }

        logger->info("[{}] -- Succeeded. Changing status to `remote`", task->id());
        task->setStatus("remote");

    } catch (SyncException & ex) {
        logger->error("[{}] -- Failed ({}). Changing status to `complete`", task->id(), ex.toJSON().dump());
        logger->flush();
        task->setError(ex.toJSON());
        task->setStatus("complete");
    }
    
    store->save(task);
}

// PerformRemote is run from the foreground worker

void TaskProcessor::performRemote(Task * task) {
    string cname = task->constructorName();

    logger->info("[{}] Running {} performRemote:", task->id(), cname);
    
    try {
        if (task->accountId() != account->id()) {
            throw SyncException("generic", "You must provide an account id.", false);
        }
        if (task->shouldCancel()) {
            task->setStatus("cancelled");
        } else {
            if (cname == "ChangeUnreadTask") {
                if (account->usesSmarterMailAPI()) performRemoteSmarterMailChange(task);
                else if (account->usesMicrosoftGraph()) performRemoteMicrosoftGraphChange(task);
                else performRemoteChangeOnMessages(task, false, _applyUnreadInIMAPFolder);
                
            } else if (cname == "ChangeStarredTask") {
                if (account->usesSmarterMailAPI()) performRemoteSmarterMailChange(task);
                else if (account->usesMicrosoftGraph()) performRemoteMicrosoftGraphChange(task);
                else performRemoteChangeOnMessages(task, false, _applyStarredInIMAPFolder);
                
            } else if (cname == "ChangeFolderTask") {
                if (account->usesSmarterMailAPI()) performRemoteSmarterMailChange(task);
                else if (account->usesMicrosoftGraph()) performRemoteMicrosoftGraphChange(task);
                else performRemoteChangeOnMessages(task, true, _applyFolderMoveInIMAPFolder);

            } else if (cname == "ChangeLabelsTask") {
                performRemoteChangeOnMessages(task, false, _applyLabelChangeInIMAPFolder);
                
            } else if (cname == "SyncbackDraftTask") {
                // right now we don't syncback drafts
                
            } else if (cname == "DestroyDraftTask") {
                performRemoteDestroyDraft(task);

            } else if (cname == "SyncbackCategoryTask") {
                performRemoteSyncbackCategory(task);
                
            } else if (cname == "DestroyCategoryTask") {
                performRemoteDestroyCategory(task);
            
            } else if (cname == "SendDraftTask") {
                performRemoteSendDraft(task);
                
            } else if (cname == "SyncbackMetadataTask") {
                performRemoteSyncbackMetadata(task);

            } else if (cname == "SendFeatureUsageEventTask") {
                performRemoteSendFeatureUsageEvent(task);

            } else if (cname == "ChangeRoleMappingTask") {
                // no-op

            } else if (cname == "ExpungeAllInFolderTask") {
                performRemoteExpungeAllInFolder(task);

            } else if (cname == "GetMessageRFC2822Task") {
                performRemoteGetMessageRFC2822(task);

            } else if (cname == "GetManyRFC2822Task") {
                performRemoteGetManyRFC2822(task);

            } else if (cname == "CrossAccountMoveFolderTask") {
                performRemoteCrossAccountMoveFolder(task);

            } else if (cname == "EventRSVPTask") {
                performRemoteSendRSVP(task);
                
            } else if (cname == "DestroyContactTask") {
                performRemoteDestroyContact(task);

            } else if (cname == "SyncbackContactTask") {
                performRemoteSyncbackContact(task);

            } else if (cname == "ChangeContactGroupMembershipTask") {
                performRemoteChangeContactGroupMembership(task);

            } else if (cname == "SyncbackContactGroupTask") {
                performRemoteSyncbackContactGroup(task);

            } else if (cname == "DestroyContactGroupTask") {
                performRemoteDestroyContactGroup(task);

            } else if (cname == "SyncbackEventTask") {
                performRemoteSyncbackEvent(task);

            } else if (cname == "DestroyEventTask") {
                performRemoteDestroyEvent(task);

            } else {
                logger->error("Unsure of how to process this task type {}", cname);
            }

            // A long-running task (e.g. GetManyRFC2822Task) can observe a
            // cancel request while it runs and stop early, setting should_cancel
            // on the task. Honor that here so it ends up "cancelled" rather than
            // "complete"; the pre-run check above only covers cancels that
            // arrive before the task starts.
            if (task->shouldCancel()) {
                logger->info("[{}] -- Cancelled. Changing status to `cancelled`", task->id());
                task->setStatus("cancelled");
            } else {
                logger->info("[{}] -- Succeeded. Changing status to `complete`", task->id());
                task->setStatus("complete");
            }
        }
    } catch (SyncException & ex) {
        logger->error("[{}] -- Failed ({}). Changing status to `complete`", task->id(), ex.toJSON().dump());
        logger->flush();
        task->setError(ex.toJSON());
        task->setStatus("complete");
    }
    store->save(task);
}

void TaskProcessor::cancel(string taskId) {
    MailStoreTransaction transaction{store, "cancel"};
    auto task = store->find<Task>(Query().equal("id", taskId).equal("accountId", account->id()));
    if (task != nullptr) {
        task->setShouldCancel();
        store->save(task.get());
    }
    transaction.commit();
}

#pragma mark Privates

Message TaskProcessor::inflateClientDraftJSON(json & draftJSON, shared_ptr<Message> existing = nullptr) {
    // set other JSON attributes the client may not have populated, but we require to be non-null
    
    // Note BG 2019 - I don't know why this is so defensive. I guess these objects JSON is created client-side
    // and we don't want the C++ to need to trust that the JS and the C++ are exactly aligned?
    
    // Followup: This is because the client only serializes the fields it's aware of, so remoteUID, etc.
    // ARE actually missing.
    
    json base;
    if (existing) {
        base = existing->_data;
    } else {
        Query q = Query().equal("accountId", account->id()).equal("role", "drafts");
        auto folder = store->find<Folder>(q);
        if (folder.get() == nullptr) {
            q = Query().equal("accountId", account->id()).equal("role", "all");
            folder = store->find<Folder>(q);
        }
        if (folder == nullptr) {
            throw SyncException("no-drafts-folder", "SummerMail can't find your Drafts folder. To create and send mail, visit Preferences > Folders and choose a Drafts folder.", false);
        }
        base = {
            {"remoteUID", 0},
            {"draft", true},
            {"unread", false},
            {"starred", false},
            {"folder", folder->toJSON()},
            {"remoteFolder", folder->toJSON()},
            {"date", time(0)},
            {"_sa", 0},
            {"_suc", 0},
            {"labels", json::array()},
            {"id", MailUtils::idForDraftHeaderMessageId(draftJSON["aid"], draftJSON["hMsgId"])},
            {"threadId", ""},
            {"gMsgId", ""},
            {"files", json::array()},
            {"from", json::array()},
            {"to", json::array()},
            {"cc", json::array()},
            {"bcc", json::array()},
            {"replyTo", json::array()},
        };
    }

    // Take the base values (either our local copy of the draft with our metadata, or a new stub) and smash in
    // the values provided by the client. This allows us to retain the actual remoteUID, remoteFolder, etc. if
    // the draft is synced remotely.
    
    draftJSON.insert(base.begin(), base.end());

    // Always update the timestamp
    draftJSON["date"] = time(0);
    
    auto msg = Message{draftJSON};
    
    if (msg.accountId() != account->id()) {
        throw SyncException("bad-accountid", "The draft in this task has the wrong account ID.", false);
    }

    return msg;
}

ChangeMailModels TaskProcessor::inflateMessages(json & data) {
    ChangeMailModels models;

    if (data.count("resolvedMessageIds")) {
        auto ids = data["resolvedMessageIds"].get<vector<string>>();
        models.messages = store->findLargeSet<Message>("id", ids);
    } else if (data.count("threadIds") && !data["threadIds"].empty()) {
        vector<string> threadIds{};
        for (auto & member : data["threadIds"]) {
            threadIds.push_back(member.get<string>());
        }
        models.messages = store->findLargeSet<Message>("threadId", threadIds);

    } else if (data.count("messageIds")) {
        vector<string> messageIds{};
        for (auto & member : data["messageIds"]) {
            messageIds.push_back(member.get<string>());
        }
        models.messages = store->findLargeSet<Message>("id", messageIds);
    }
    
    models.messages.erase(remove_if(models.messages.begin(), models.messages.end(), [&](const shared_ptr<Message> & msg) {
        return msg->accountId() != account->id();
    }), models.messages.end());
    return models;
}

void TaskProcessor::performLocalChangeOnMessages(Task * task, void (*modifyLocalMessage)(Message *, json &)) {
    MailStoreTransaction transaction{store, "performLocalChangeOnMessages"};
    
    json & data = task->data();
    ChangeMailModels models = inflateMessages(data);
    const auto threadMessages = models.messages;
    if (task->constructorName() == "ChangeFolderTask" && data.value("preserveSent", false)) {
        // Archive is not an explicit move: retain physical Sent copies, regardless
        // of sender aliases or localized/custom Sent-folder names.
        set<string> sentFolders;
        for (const auto & folder : store->findAll<Folder>(Query().equal("accountId", account->id()).equal("role", "sent")))
            sentFolders.insert(folder->id());
        models.messages.erase(remove_if(models.messages.begin(), models.messages.end(), [&](const shared_ptr<Message> & msg) {
            return sentFolders.count(msg->clientFolderId()) || sentFolders.count(msg->remoteFolderId());
        }), models.messages.end());
    }
    // Persist exactly the messages whose optimistic changes/locks we own. Thread
    // repairs and incoming replies must not change the remote operation's scope.
    data["resolvedMessageIds"] = json::array();
    for (const auto & msg : models.messages) data["resolvedMessageIds"].push_back(msg->id());
    store->save(task);
    bool recomputeThreadAttributes = data.count("threadIds") && !data["threadIds"].empty();
    
    for (auto msg : models.messages) {
        // TEMPORARY
        if (recomputeThreadAttributes) {
            msg->_skipThreadUpdatesAfterSave = true;
        }
        
        // perform local changes
        modifyLocalMessage(msg.get(), data);

        // prevent remote changes to this message for 24 hours
        // so the changes aren't reverted by sync before we can syncback.
        msg->setSyncUnsavedChanges(msg->syncUnsavedChanges() + 1);
        msg->setSyncedAt(time(0) + 24 * 60 * 60);

        store->save(msg.get());
    }

    // TEMPORARY
    // if we were given a set of threadIds, we might as well rebalance the counters
    // and correct any refcounting issues the user may be seeing, since we already
    // have all the messages in memory.
    if (recomputeThreadAttributes) {
        vector<string> threadIds{};
        for (auto & member : data["threadIds"]) {
            threadIds.push_back(member.get<string>());
        }
        auto chunks = MailUtils::chunksOfVector(threadIds, 500);
        auto allLabels = store->allLabelsCache(task->accountId());

        for (auto chunk : chunks) {
            auto threads = store->findAllMap<Thread>(Query().equal("id", chunk), "id");

            for (auto pair : threads) {
                pair.second->resetCountedAttributes();
            }
            for (auto msg : threadMessages) {
                if (threads.count(msg->threadId())) {
                    threads[msg->threadId()]->applyMessageAttributeChanges(MessageEmptySnapshot, msg.get(), allLabels);
                }
            }
            for (auto pair : threads) {
                store->save(pair.second.get());
            }
        }
    }
    // END TEMPORARY

    transaction.commit();
}

void TaskProcessor::settleMessageChanges(const vector<shared_ptr<Message>> & messages, bool updatesFolder, bool failed) {
    MailStoreTransaction transaction{store, "settleMessageChanges"};
    for (const auto & unsafe : messages) {
        auto safe = store->find<Message>(Query().equal("id", unsafe->id()).equal("accountId", account->id()));
        if (!safe) continue;
        if (updatesFolder) {
            safe->setRemoteUID(unsafe->remoteUID());
            safe->setRemoteFolder(unsafe->remoteFolder());
            safe->setGraphId(unsafe->graphId());
        }
        int remaining = max(0, safe->syncUnsavedChanges() - 1);
        safe->setSyncUnsavedChanges(remaining);
        if (remaining == 0) {
            safe->setSyncedAt(failed ? 0 : time(0));
            if (updatesFolder) {
                Folder actual{safe->remoteFolder()};
                safe->setClientFolder(&actual);
            }
        }
        store->save(safe.get());
    }
    // Publish corrected folder/counter state, including partially completed moves.
    transaction.commit();
}

void TaskProcessor::performRemoteChangeOnMessages(Task * task, bool updatesFolder, void (*applyInFolder)(IMAPSession * session, String * path, IndexSet * uids, vector<shared_ptr<Message>> messages, json & data)) {
    // Perform the remote action on the impacted messages
    json & data = task->data();
    
    // Grab the messages, group into folders, and perform the remote changes.
    // Note that we reload the messages to update them locally because
    // this code does I/O and is not inside a transaction! Other task
    // performLocal calls could be happening at the same time.
    vector<shared_ptr<Message>> messages = inflateMessages(data).messages;
    map<string, vector<shared_ptr<Message>>> msgsByFolder{};

    for (auto msg : messages) {
        string path = msg->remoteFolder()["path"].get<string>();
        msgsByFolder[path].push_back(msg);
    }
    
    set<string> completed;
    auto persistBatch = [&](const vector<shared_ptr<Message>> & batch, bool failed) {
        settleMessageChanges(batch, updatesFolder, failed);
    };
    try {
        for (auto & pair : msgsByFolder) {
            // Bound command length, verification work and recovery to 100 UIDs.
            for (auto batch : MailUtils::chunksOfVector(pair.second, 100)) {
                IndexSet * uids = IndexSet::indexSet();
                for (auto msg : batch) uids->addIndex(msg->remoteUID());
                if (!updatesFolder || pair.first != data["folder"]["path"].get<string>()) {
                    applyInFolder(session, AS_MCSTR(pair.first), uids, batch, data);
                }
                persistBatch(batch, false);
                for (auto msg : batch) completed.insert(msg->id());
            }
        }
    } catch (const SyncException &) {
        // Preserve confirmed mappings even if a later batch failed. Release only
        // this task's optimistic locks; a newer queued action still owns its lock.
        vector<shared_ptr<Message>> pending;
        for (auto msg : messages) if (!completed.count(msg->id())) pending.push_back(msg);
        persistBatch(pending, true);
        throw;
    }
}

void TaskProcessor::performRemoteMicrosoftGraphChange(Task * task) {
    json & data = task->data();
    string cname = task->constructorName();
    auto messages = inflateMessages(data).messages;
    shared_ptr<Folder> destination = nullptr;
    set<string> completed;
    try {
    auto token = SharedXOAuth2TokenManager()->partsForAccount(account).accessToken;
    if (cname == "ChangeFolderTask") {
        destination = store->find<Folder>(Query().equal("id", data["folder"]["id"].get<string>()));
        if (!destination || !destination->localStatus().count("graphId")) {
            throw SyncException("invalid-graph-folder", "The Microsoft Graph destination folder is unavailable.", false);
        }
    }

    for (auto & message : messages) {
        if (destination && message->remoteFolderId() == destination->id()) {
            settleMessageChanges({message}, true, false);
            completed.insert(message->id());
            continue;
        }
        if (message->graphId().empty())
            throw SyncException("move-unresolved", "A message has no server identity. Synchronize and retry.", false);
        string url = MicrosoftGraphBaseURL(account) + "/messages/" + taskGraphUrlEncode(message->graphId());
        json payload;
        string method = "PATCH";
        if (cname == "ChangeUnreadTask") {
            payload = {{"isRead", !data["unread"].get<bool>()}};
        } else if (cname == "ChangeStarredTask") {
            payload = {{"flag", {{"flagStatus", data["starred"].get<bool>() ? "flagged" : "notFlagged"}}}};
        } else if (cname == "ChangeFolderTask") {
            method = "POST";
            url += "/move";
            payload = {{"destinationId", destination->localStatus()["graphId"]}};
        }
        string serialized = payload.dump();
        json response = PerformJSONRequest(CreateMicrosoftGraphRequest(url, method, token, serialized.c_str()));
        if (destination) {
            if (!response.count("id") || !response["id"].is_string() || response["id"].get<string>().empty() ||
                response.value("parentFolderId", string()) != destination->localStatus()["graphId"].get<string>())
                throw SyncException("move-unconfirmed", "The server did not confirm the moved message. Refresh both folders before retrying.", false);
            message->setGraphId(response["id"].get<string>());
            message->setRemoteFolder(destination.get());
        }
        settleMessageChanges({message}, destination != nullptr, false);
        completed.insert(message->id());
    }
    } catch (...) {
        vector<shared_ptr<Message>> pending;
        for (auto & msg : messages) if (!completed.count(msg->id())) pending.push_back(msg);
        settleMessageChanges(pending, cname == "ChangeFolderTask", true);
        throw;
    }
}

void TaskProcessor::performRemoteSmarterMailChange(Task * task) {
    json & data = task->data();
    string cname = task->constructorName();
    auto messages = inflateMessages(data).messages;
    SmarterMailClient client(account);
    shared_ptr<Folder> destination = nullptr;
    set<string> completed;
    try {
    if (cname == "ChangeFolderTask") {
        destination = store->find<Folder>(Query().equal("id", data["folder"]["id"].get<string>()));
        if (!destination) throw SyncException("invalid-smartermail-folder", "The SmarterMail destination folder is unavailable.", false);
    }

    map<string, vector<shared_ptr<Message>>> messagesByFolder;
    bool unresolvedIdentity = false;
    for (auto & message : messages) {
        if (destination && message->remoteFolderId() == destination->id()) {
            settleMessageChanges({message}, true, false);
            completed.insert(message->id());
            continue;
        }
        const string path = message->remoteFolder().value("path", "");
        if (!MoveResult::uid(message->remoteUID()) || path.empty()) { unresolvedIdentity = true; continue; }
        messagesByFolder[path].push_back(message);
    }
    for (auto & entry : messagesByFolder) {
        for (auto batch : MailUtils::chunksOfVector(entry.second, 100)) {
            vector<uint32_t> uids;
            for (auto & msg : batch) uids.push_back(msg->remoteUID());
            if (cname == "ChangeUnreadTask") client.markRead(entry.first, uids, !data["unread"].get<bool>());
            else if (cname == "ChangeStarredTask") client.setFlagged(entry.first, uids, data["starred"].get<bool>());
            else if (destination) {
                // SmarterMail confirms a delete-to-Trash through delete-messages,
                // but does not return the destination UID mapping supplied by a
                // regular move. Do not route deletes through the move path and
                // turn a successful server delete into an "unconfirmed" error.
                // The sentinel keeps the item out of further mutations until the
                // next Trash refresh reconciles its server-assigned UID.
                if (destination->role() == "trash") {
                    client.remove(entry.first, uids, true);
                    for (auto & msg : batch) {
                        msg->setRemoteFolder(destination.get());
                        msg->setRemoteUID(UINT32_MAX - 1);
                    }
                    settleMessageChanges(batch, true, false);
                    for (auto & msg : batch) completed.insert(msg->id());
                    continue;
                }
                const auto response = client.move(entry.first, uids, destination->path());
                vector<shared_ptr<Message>> accepted;
                set<uint32_t> assigned;
                size_t awaitingReconciliation = 0;
                for (auto & msg : batch) {
                    uint32_t uid = MoveResult::mapped(response, msg->remoteUID());
                    msg->setRemoteFolder(destination.get());
                    if (uid && assigned.insert(uid).second) {
                        msg->setRemoteUID(uid);
                    } else {
                        // A successful SmarterMail move commonly omits the new UID.
                        // Do not scan a potentially huge destination folder or report
                        // a false failure. The sentinel prevents later mutations from
                        // targeting the stale source UID while normal bounded folder
                        // synchronization discovers the authoritative destination row.
                        msg->setRemoteUID(UINT32_MAX - 1);
                        awaitingReconciliation++;
                    }
                    accepted.push_back(msg);
                }
                settleMessageChanges(accepted, true, false);
                for (auto & msg : accepted) completed.insert(msg->id());
                if (awaitingReconciliation) {
                    logger->info("SmarterMail accepted {} move(s) without destination UID mappings; queued for reconciliation.", awaitingReconciliation);
                }
                continue;
            }
            settleMessageChanges(batch, false, false);
            for (auto & msg : batch) completed.insert(msg->id());
        }
    }
    if (unresolvedIdentity) {
        if (cname == "ChangeFolderTask")
            throw SyncException("move-unconfirmed", "Some messages do not yet have usable server identities. Refresh both folders before retrying.", false);
        throw SyncException("message-identity-unavailable", "Some messages need to synchronize before this change can be applied.", false);
    }
    } catch (...) {
        vector<shared_ptr<Message>> pending;
        for (auto & msg : messages) if (!completed.count(msg->id())) pending.push_back(msg);
        settleMessageChanges(pending, cname == "ChangeFolderTask", true);
        throw;
    }
}

void TaskProcessor::performLocalSaveDraft(Task * task) {
    json & draftJSON = task->data()["draft"];
    
    {
        MailStoreTransaction transaction{store, "performLocalSaveDraft"};

        // If the draft already exists, we need to visibly change it's attributes
        // to trigger the correct didSave hooks, etc. Find and update it.
        shared_ptr<Message> existing = nullptr;
        if (draftJSON.count("id")) {
            existing = store->find<Message>(Query().equal("id", draftJSON["id"].get<string>()));
        }

        Message draft = inflateClientDraftJSON(draftJSON, existing);

        if (existing) {
            // NOTE: to accept all changes we just swap the data BUT, the new data
            // may have an outdated version and the version dicates whether we
            // INSERT or UPDATE. It's critical we bump the version of `existing`.
            int existingVersion = existing->version();
            existing->_data = draft._data;
            existing->_data["v"] = existingVersion + 1;
            store->save(existing.get());
        } else {
            store->save(&draft);
        }

        if (draftJSON.count("body")) {
            SQLite::Statement insert(store->db(), "REPLACE INTO MessageBody (id, value) VALUES (?, ?)");
            insert.bind(1, draft.id());
            insert.bind(2, draftJSON["body"].get<string>());
            insert.exec();
        }
        transaction.commit();
    }
}

void TaskProcessor::performLocalDestroyDraft(Task * task) {
    vector<string> messageIds = task->data()["messageIds"];

    logger->info("-- Hiding / detatching drafts while they're deleted...");
    
    // Find the trash folder
    auto trash = store->find<Folder>(Query().equal("accountId", account->id()).equal("role", "trash"));
    if (trash == nullptr) {
        throw SyncException("no-trash-folder", "SummerMail doesn't know which folder to use for trash. Visit Preferences > Folders to assign a trash folder.", false);
    }

    auto stubIds = json::array();
    
    // we need to free up the draft ID immediately because the user can
    // switch a draft between accounts, and we may need to re-create a
    // new draft with the same acctId + hMsgId combination.
    
    // Destroy drafts locally and create stubs that prevent the sync
    // worker from replacing them while we delete them via IMAP.
    {
        MailStoreTransaction transaction{store, "performLocalDestroyDraft"};

        auto drafts = store->findLargeSet<Message>("id", messageIds);
        for (auto & draft : drafts) {
            store->remove(draft.get());
            
            auto stub = Message::messageWithDeletionPlaceholderFor(draft);
            stub->setClientFolder(trash.get());
            store->save(stub.get());
            stubIds.push_back(stub->id());

            logger->info("-- Replacing local ID {} with {}", draft->id(), stub->id());
        }

        transaction.commit();
    }
    task->data()["stubIds"] = stubIds;
}

void TaskProcessor::performRemoteDestroyDraft(Task * task) {
    vector<string> stubIds = task->data()["stubIds"];
    auto stubs = store->findLargeSet<Message>("id", stubIds);


    for (auto & stub : stubs) {
        if (stub->remoteUID() == 0) {
            continue; // not synced to server at all
        }
        if (account->usesSmarterMailAPI()) {
            SmarterMailClient client(account);
            client.remove(stub->remoteFolder().value("path", ""), {stub->remoteUID()});
            store->remove(stub.get());
            continue;
        }
        if (account->usesMicrosoftGraph()) {
            if (!stub->graphId().empty()) {
                auto token = SharedXOAuth2TokenManager()->partsForAccount(account).accessToken;
                string url = MicrosoftGraphBaseURL(account) + "/messages/" + taskGraphUrlEncode(stub->graphId());
                PerformJSONRequest(CreateMicrosoftGraphRequest(url, "DELETE", token));
            }
            store->remove(stub.get());
            continue;
        }
        auto uids = IndexSet::indexSetWithIndex(stub->remoteUID());
        String * path = AS_MCSTR(stub->remoteFolder()["path"].get<string>());

        logger->info("-- Deleting remote draft {}", stub->id());
        _removeMessagesResilient(session, store, account->id(), path, uids);

        // remove the stub from our local cache - would eventually get removed
        // during sync, but we don't want to fetch it's body or anything
        store->remove(stub.get());
    }
}

void TaskProcessor::performLocalDestroyContact(Task * task) {
    vector<string> contactIds {};
    for (json & c : task->data()["contacts"]) {
        contactIds.push_back(c["id"].get<string>());
    }

    {
        MailStoreTransaction transaction{store, "performLocalDestroyContact"};
        auto deleted = store->findLargeSet<Contact>("id", contactIds);
        for (auto & c : deleted) {
            c->setHidden(true);
            store->save(c.get());
        }
        transaction.commit();
    }
}

void TaskProcessor::performRemoteDestroyContact(Task * task) {
    vector<string> contactIds {};
    for (json & c : task->data()["contacts"]) {
        contactIds.push_back(c["id"].get<string>());
    }

    if (account->provider() == "gmail") {
        auto deleted = store->findLargeSet<Contact>("id", contactIds);
        auto gpeople = make_shared<GoogleContactsWorker>(account);
        for (auto & contact : deleted) {
            gpeople->deleteContact(contact);
        }
    } else {

        auto deleted = store->findLargeSet<Contact>("id", contactIds);
        auto dav = make_shared<DAVWorker>(account);
        for (auto & contact : deleted) {
            dav->deleteContact(contact);
        }
    }
}

void TaskProcessor::performLocalSyncbackContactGroup(Task * task) {
    string id = task->data()["group"].count("id") ? task->data()["group"]["id"].get<string>() : "";
    string name = task->data()["group"]["name"].get<string>();
    shared_ptr<ContactBook> book = store->find<ContactBook>(Query().equal("accountId", account->id()));

    if (account->provider() == "gmail") {
        // Create or update ContactGroup
        if (id == "") {
            id = MailUtils::idRandomlyGenerated();
            task->data()["group"]["id"] = id;
            store->save(task);
        }
        auto local = store->find<ContactGroup>(Query().equal("id", id));
        if (!local) {
            local = make_shared<ContactGroup>(id, account->id());
        }
        local->setBookId(book->id());
        local->setName(name);
        store->save(local.get());
        
    } else {
        if (id != "") {
            // Update ContactGroup
            auto existing = store->find<ContactGroup>(Query().equal("id", id));
            if (!existing) {
                return;
            }
            existing->setName(name);
            store->save(existing.get());
            
            // Update underlying contact and VCF
            auto contact = store->find<Contact>(Query().equal("id", id));
            contact->setName(name);
            contact->mutateCardInInfo([&](shared_ptr<VCard> card) {
                if (card->getName()) {
                    card->getName()->setValue(name);
                }
                if (card->getFormattedName()) {
                    card->getFormattedName()->setValue(name);
                }
            });
            store->save(contact.get());
        } else {
            // Create vcf and autogen Contact and ContactGroup
            auto uid = MailUtils::idRandomlyGenerated();
            auto contact = make_shared<Contact>(uid, account->id(), "", CONTACT_MAX_REFS,
                account->usesSmarterMailAPI() ? SMARTERMAIL_SYNC_SOURCE : CARDDAV_SYNC_SOURCE);
            contact->setInfo(json::object({{"vcf", "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:"+uid+"\r\nEND:VCARD\r\n"}, {"href", ""}}));
            contact->setHidden(true);
            contact->setName(name);
            contact->mutateCardInInfo([&](shared_ptr<VCard> vcard) {
                vcard->setName(name);
                vcard->addProperty(make_shared<VCardProperty>("FN", name));
                vcard->addProperty(make_shared<VCardProperty>(X_VCARD3_KIND, "group"));
            });

            task->data()["group"]["id"] = uid;
            store->save(task);

            store->save(contact.get());
            auto dav = make_shared<DAVWorker>(account);
            dav->rebuildContactGroup(contact);
        }
    }
}

void TaskProcessor::performRemoteSyncbackContactGroup(Task * task) {
    string id = task->data()["group"].count("id") ? task->data()["group"]["id"].get<string>() : "";
    if (id == "") {
        logger->error("performRemoteSyncbackContactGroup: Group did not get assigned an ID.");
        return;
    }

    if (account->provider() == "gmail") {
        auto group = store->find<ContactGroup>(Query().equal("id", id));
        auto gpeople = make_shared<GoogleContactsWorker>(account);
        gpeople->upsertContactGroup(group);
    } else {
        auto contact = store->find<Contact>(Query().equal("id", id));
        auto dav = make_shared<DAVWorker>(account);
        dav->writeAndResyncContact(contact);
    }
}

void TaskProcessor::performLocalDestroyContactGroup(Task * task) {
    string id = task->data()["group"]["id"].get<string>();
    auto deleted = store->find<ContactGroup>(Query().equal("id", id));
    if (!deleted) return;
    
    if (account->provider() == "gmail") {
        task->data()["googleResourceName"] = deleted->googleResourceName();
        store->save(task);
    }
    store->remove(deleted.get());
}

void TaskProcessor::performRemoteDestroyContactGroup(Task * task) {
    string id = task->data()["group"]["id"].get<string>();

    if (account->provider() == "gmail") {
        if (!task->data().count("googleResourceName")) {
            logger->error("performRemoteDestroyContactGroup: Group did not have a googleResourceName.");
            return;
        }
        auto resourceName = task->data()["googleResourceName"].get<string>();
        auto gpeople = make_shared<GoogleContactsWorker>(account);
        gpeople->deleteContactGroup(resourceName);
    } else {
        auto contact = store->find<Contact>(Query().equal("id", id));
        if (!contact) return;
        auto dav = make_shared<DAVWorker>(account);
        dav->deleteContact(contact);
    }
}


void TaskProcessor::performLocalChangeContactGroupMembership(Task * task) {
    vector<string> contactIds {};
    for (json & c : task->data()["contacts"]) {
        contactIds.push_back(c["id"].get<string>());
    }
    auto contacts = store->findLargeSet<Contact>("id", contactIds);
    auto direction = task->data()["direction"].get<string>();
    auto groupId = task->data()["group"]["id"].get<string>();
    
    if (account->provider() == "gmail") {
        auto group = store->find<ContactGroup>(Query().equal("id", groupId));
        auto groupMemberIds = group->getMembers(store);
        if (direction == "add") {
            groupMemberIds.insert(groupMemberIds.end(), contactIds.begin(), contactIds.end());
        } else {
            for (auto id : contactIds) {
                auto pos = std::find(groupMemberIds.begin(), groupMemberIds.end(), id);
                if (pos != groupMemberIds.end()) groupMemberIds.erase(pos);
            }
        }
        group->syncMembers(store, groupMemberIds);

    } else {
        auto contactForGroup = store->find<Contact>(Query().equal("id", groupId).equal("accountId", account->id()));
       
        contactForGroup->mutateCardInInfo([&](shared_ptr<VCard> card) {
            if (direction == "add") {
                DAVUtils::addMembersToGroupCard(card, contacts);
            } else {
                DAVUtils::removeMembersFromGroupCard(card, contacts);
            }
        });
        
        auto dav = make_shared<DAVWorker>(account);
        dav->rebuildContactGroup(contactForGroup);
        store->save(contactForGroup.get());
    }
}


void TaskProcessor::performRemoteChangeContactGroupMembership(Task * task) {
    auto groupId = task->data()["group"]["id"].get<string>();
    
    if (account->provider() == "gmail") {
        auto direction = task->data()["direction"].get<string>();
        vector<string> contactIds {};
        for (json & c : task->data()["contacts"]) {
            contactIds.push_back(c["id"].get<string>());
        }
        auto contacts = store->findLargeSet<Contact>("id", contactIds);
        auto group = store->find<ContactGroup>(Query().equal("id", groupId));
        auto gpeople = make_shared<GoogleContactsWorker>(account);
        gpeople->updateContactGroupMembership(group, contacts, direction);

    } else {
        auto contactForGroup = store->find<Contact>(Query().equal("id", groupId).equal("accountId", account->id()));
        auto dav = make_shared<DAVWorker>(account);
        dav->writeAndResyncContact(contactForGroup);
    }
}


void TaskProcessor::performLocalSyncbackContact(Task * task) {
    auto clientside = make_shared<Contact>(task->data()["contact"]);
    auto source = account->provider() == "gmail" ? GOOGLE_SYNC_SOURCE :
        (account->usesSmarterMailAPI() ? SMARTERMAIL_SYNC_SOURCE : CARDDAV_SYNC_SOURCE);
    if (clientside->source() != "" && clientside->source() != source) {
        logger->error("performLocalSyncbackContact: Client picked incorrect source for new contact: {} != {}", source, clientside->source());
        return;
    }

    auto local = task->data()["contact"].count("id")
        ? store->find<Contact>(Query().equal("id", clientside->id()))
        : make_shared<Contact>(MailUtils::idRandomlyGenerated(), account->id(), "", CONTACT_MAX_REFS, source);

    // Note: The client may not be aware of all of the key/value pairs we store in contact JSON,
    // so it's JSON in the task may omit some properties. To make sure we don't damage the
    // contact, find and update only the allowed attributes.
    if (account->usesSmarterMailAPI()) {
        json merged = local->info();
        json clientInfo = clientside->info();
        for (auto item = clientInfo.begin(); item != clientInfo.end(); ++item) {
            merged[item.key()] = item.value();
        }
        local->setInfo(merged);
    } else {
        local->setInfo(clientside->info());
    }
    local->setName(clientside->name());
    local->setEmail(clientside->email());
    store->save(local.get());
    
    task->data()["contact"]["id"] = local->id();
    store->save(task);
}

void TaskProcessor::performRemoteSyncbackContact(Task * task) {
    string id = task->data()["contact"]["id"].get<string>();
    auto contact = store->find<Contact>(Query().equal("id", id).equal("accountId", account->id()));
    if (contact == nullptr) {
        throw SyncException("not-found", "Contact not found for syncback", false);
    }

    if (contact->source() == CONTACT_SOURCE_MAIL) {
        return;
    }

    if (account->provider() == "gmail") {
        auto gpeople = make_shared<GoogleContactsWorker>(account);
        gpeople->upsertContact(contact);
    } else {
        auto dav = make_shared<DAVWorker>(account);
        dav->writeAndResyncContact(contact);
    }
}


void TaskProcessor::performLocalSyncbackEvent(Task * task) {
    json & eventJSON = task->data()["event"];
    string calendarId = task->data()["calendarId"].get<string>();

    // Check if event already exists
    string eventId = eventJSON.count("id") ? eventJSON["id"].get<string>() : "";
    shared_ptr<Event> existing = nullptr;

    if (eventId != "") {
        existing = store->find<Event>(Query().equal("id", eventId));
    }

    if (existing) {
        // UPDATE: Merge client changes into existing event
        if (eventJSON.count("ics")) {
            existing->setIcsData(eventJSON["ics"].get<string>());
            // Re-parse ICS to update recurrence fields
            ICalendar cal(existing->icsData());
            if (!cal.Events.empty()) {
                // Find the VEVENT matching our event's recurrenceId
                string eventRecurrenceId = existing->recurrenceId();
                ICalendarEvent* matchingEvent = nullptr;

                for (auto icsEvent : cal.Events) {
                    if (icsEvent->RecurrenceId == eventRecurrenceId) {
                        matchingEvent = icsEvent;
                        break;
                    }
                }

                // Fall back to first event if no match
                if (!matchingEvent) {
                    matchingEvent = cal.Events.front();
                }

                existing->_data["rs"] = matchingEvent->DtStart.toUnix();
                existing->_data["re"] = endOf(matchingEvent).toUnix();
                existing->_data["icsuid"] = matchingEvent->UID;
                existing->setRecurrenceId(matchingEvent->RecurrenceId);
                existing->setStatus(matchingEvent->Status.empty() ? "CONFIRMED" : matchingEvent->Status);
            }
        }
        store->save(existing.get());
        task->data()["event"]["id"] = existing->id();
    } else {
        // CREATE: Generate new event with temporary ID
        string tempId = MailUtils::idRandomlyGenerated();
        string icsData = eventJSON["ics"].get<string>();
        ICalendar cal(icsData);

        if (cal.Events.empty()) {
            throw SyncException("invalid-ics", "ICS data does not contain any events", false);
        }

        // For new event creation, use the first VEVENT (typically only one)
        // The Event constructor now handles recurrenceId from the ICalendarEvent
        auto icsEvent = cal.Events.front();
        Event event("", account->id(), calendarId, icsData, icsEvent);
        event._data["id"] = tempId;  // Temporary ID until server assigns etag
        store->save(&event);

        task->data()["event"]["id"] = tempId;
    }

    store->save(task);
}

void TaskProcessor::performRemoteSyncbackEvent(Task * task) {
    string eventId = task->data()["event"]["id"].get<string>();
    auto event = store->find<Event>(Query().equal("id", eventId).equal("accountId", account->id()));

    if (event == nullptr) {
        throw SyncException("not-found", "Event not found for syncback", false);
    }

    auto dav = make_shared<DAVWorker>(account);
    dav->writeAndResyncEvent(event);
}

void TaskProcessor::performLocalDestroyEvent(Task * task) {
    vector<string> eventIds {};
    for (json & e : task->data()["events"]) {
        eventIds.push_back(e["id"].get<string>());
    }

    // Mark events as hidden locally (they'll be fully removed after remote delete succeeds)
    // Note: Unlike contacts, we don't have a "hidden" field on events, so we just
    // leave them in place until performRemote completes.
}

void TaskProcessor::performRemoteDestroyEvent(Task * task) {
    vector<string> eventIds {};
    for (json & e : task->data()["events"]) {
        eventIds.push_back(e["id"].get<string>());
    }

    auto events = store->findLargeSet<Event>("id", eventIds);
    auto dav = make_shared<DAVWorker>(account);

    for (auto & event : events) {
        dav->deleteEvent(event);
    }
}


void TaskProcessor::performLocalSyncbackCategory(Task * task) {
    
}

void TaskProcessor::performRemoteSyncbackCategory(Task * task) {
    json & data = task->data();
    string accountId = task->accountId();
    string path = data["path"].get<string>();
    string existingPath = data.count("existingPath") ? data["existingPath"].get<string>() : "";

    if (account->usesSmarterMailAPI()) {
        SmarterMailClient client(account);
        if (existingPath.empty()) client.createFolder(path);
        else client.renameFolder(existingPath, path);
        string localId = MailUtils::idForFolder(accountId, "smartermail:" + path);
        auto localModel = existingPath.empty()
            ? shared_ptr<Folder>()
            : store->find<Folder>(Query().equal("accountId", accountId).equal("path", existingPath));
        if (!localModel) localModel = make_shared<Folder>(localId, accountId, 0);
        localModel->setPath(path);
        localModel->setRole("");
        localModel->localStatus()["smarterMailPath"] = path;
        data["created"] = localModel->toJSON();
        store->save(localModel.get());
        return;
    }

    if (account->usesMicrosoftGraph()) {
        auto token = SharedXOAuth2TokenManager()->partsForAccount(account).accessToken;
        string displayName = path.substr(path.find_last_of('/') == string::npos ? 0 : path.find_last_of('/') + 1);
        json payload = {{"displayName", displayName}};
        string serialized = payload.dump();
        json remote;
        shared_ptr<Folder> localModel = nullptr;
        if (!existingPath.empty()) {
            localModel = store->find<Folder>(Query().equal("accountId", accountId).equal("path", existingPath));
            if (!localModel || !localModel->localStatus().count("graphId")) {
                throw SyncException("invalid-graph-folder", "The Microsoft Graph folder to rename was not found.", false);
            }
            string url = MicrosoftGraphBaseURL(account) + "/mailFolders/" +
                taskGraphUrlEncode(localModel->localStatus()["graphId"].get<string>());
            remote = PerformJSONRequest(CreateMicrosoftGraphRequest(url, "PATCH", token, serialized.c_str()));
        } else {
            string parentPath = path.find_last_of('/') == string::npos ? "" : path.substr(0, path.find_last_of('/'));
            string url = MicrosoftGraphBaseURL(account) + "/mailFolders";
            if (!parentPath.empty()) {
                auto parent = store->find<Folder>(Query().equal("accountId", accountId).equal("path", parentPath));
                if (!parent || !parent->localStatus().count("graphId")) {
                    throw SyncException("invalid-graph-folder", "The Microsoft Graph parent folder was not found.", false);
                }
                url += "/" + taskGraphUrlEncode(parent->localStatus()["graphId"].get<string>()) + "/childFolders";
            }
            remote = PerformJSONRequest(CreateMicrosoftGraphRequest(url, "POST", token, serialized.c_str()));
            string graphId = remote.value("id", "");
            if (graphId.empty()) throw SyncException("invalid-graph-folder", "Microsoft Graph did not return the created folder.", false);
            localModel = make_shared<Folder>(MailUtils::idForFolder(accountId, "graph:" + graphId), accountId, 0);
            localModel->localStatus()["graphId"] = graphId;
        }
        localModel->setPath(path);
        localModel->setRole("");
        data["created"] = localModel->toJSON();
        store->save(localModel.get());
        return;
    }

    // if the requested path includes "/" delimiters, replace them with the real delimiter
    char delimiter = session->defaultNamespace()->mainDelimiter();
    std::replace(path.begin(), path.end(), '/', delimiter);

    // if the requested path is missing the namespace prefix, add it
    // note: the prefix may or may not end with the delimiter character
    string mainPrefix = MailUtils::namespacePrefixOrBlank(session);
    if (mainPrefix != "" && path.find(mainPrefix) != 0) {
        if (mainPrefix[mainPrefix.length() - 1] == delimiter) {
            path = mainPrefix + path;
        } else {
            path = mainPrefix + delimiter + path;
        }
    }
    
    ErrorCode err = ErrorCode::ErrorNone;

    if (existingPath != "") {
        session->renameFolder(AS_MCSTR(existingPath), AS_MCSTR(path), &err);
    } else {
        session->createFolder(AS_MCSTR(path), &err);
    }
    
    if (err != ErrorNone) {
        data["created"] = nullptr;
        logger->error("Syncback of folder/label '{}' failed.", path);
        throw SyncException(err, "create/renameFolder");
    }
    
    // must go beneath the first use of session above.
    bool isGmail = session->storedCapabilities()->containsIndex(IMAPCapabilityGmail);
    shared_ptr<Folder> localModel = nullptr;
    
    if (existingPath != "") {
        auto query = Query().equal("accountId", accountId).equal("id", MailUtils::idForFolder(accountId, existingPath));
        localModel = isGmail ? store->find<Label>(query) : store->find<Folder>(query);
    }

    if (!localModel) {
        string id = MailUtils::idForFolder(accountId, path);
        localModel = isGmail ? make_shared<Label>(id, accountId, 0) : make_shared<Folder>(id, accountId, 0);
    }

    localModel->setPath(path);
    data["created"] = localModel->toJSON();
    store->save(localModel.get());
    
    logger->info("Syncback of folder/label '{}' succeeded.", path);
}


void TaskProcessor::performLocalSyncbackMetadata(Task * task) {
    json & data = task->data();
    string aid = task->accountId();
    string id = data["modelId"];
    string type = data["modelClassName"];
    string pluginId = data["pluginId"];
    json & value = data["value"];
    
    {
        MailStoreTransaction transaction{store, "performLocalSyncbackMetadata"};
        
        auto model = store->findGeneric(type, Query().equal("id", id).equal("accountId", aid));
        if (model) {
            int metadataVersion = model->upsertMetadata(pluginId, value);
            data["modelMetadataNewVersion"] = metadataVersion;
            store->save(model.get());
        } else {
            logger->info("cannot apply metadata locally because no model matching type:{} id:{}, aid:{} could be found.", type, id, aid);
        }

        transaction.commit();
    }
}


void TaskProcessor::performRemoteSyncbackMetadata(Task * task) {
    if (Identity::GetGlobal() == nullptr) {
        logger->info("Skipped metadata sync, not logged in.");
        return;
    }
    
    json & data = task->data();
    string id = data["modelId"];
    string pluginId = data["pluginId"];

    json payload = {
        {"objectType", data["modelClassName"]},
        {"version", data["modelMetadataNewVersion"]},
        {"value", data["value"]},
    };

    if (data["modelHeaderMessageId"].is_string()) {
        payload["headerMessageId"] = data["modelHeaderMessageId"].get<string>();
    }

    const json results = PerformIdentityRequest("/metadata/" + account->id() + "/" + id + "/" + pluginId, "POST", payload);
    logger->info("Syncback of metadata {}:{} = {} succeeded.", id, pluginId, payload.dump());
}

void TaskProcessor::performRemoteDestroyCategory(Task * task) {
    json & data = task->data();
    string accountId = task->accountId();
    string path = data["path"].get<string>();
    if (account->usesSmarterMailAPI()) {
        SmarterMailClient(account).deleteFolder(path);
        return;
    }
    if (account->usesMicrosoftGraph()) {
        auto folder = store->find<Folder>(Query().equal("accountId", accountId).equal("path", path));
        if (!folder || !folder->localStatus().count("graphId")) return;
        auto token = SharedXOAuth2TokenManager()->partsForAccount(account).accessToken;
        string url = MicrosoftGraphBaseURL(account) + "/mailFolders/" +
            taskGraphUrlEncode(folder->localStatus()["graphId"].get<string>());
        PerformJSONRequest(CreateMicrosoftGraphRequest(url, "DELETE", token));
        return;
    }
    ErrorCode err = ErrorCode::ErrorNone;
    
    session->deleteFolder(AS_MCSTR(path), &err);
    
    if (err != ErrorNone) {
        throw SyncException(err, "deleteFolder");
    }
    
    logger->info("Deletion of folder/label '{}' succeeded.", path);
}

void TaskProcessor::performRemoteSendDraft(Task * task) {
    AutoreleasePool pool;
    ErrorCode err = ErrorNone;

    // We never intend for a send task to run more than once. We set this bit
    // to ensure that - even if we don't report failures properly - we never
    // get a send task "stuck" in the queue sending over and over. All retries
    // are user-triggered and create a new task.
    if (task->data().count("_performRemoteRan")) { return; }
    task->data()["_performRemoteRan"] = true;
    store->save(task);

    // load the draft and body from the task
    json & draftJSON = task->data()["draft"];
    json & perRecipientBodies = task->data()["perRecipientBodies"];
    string body = draftJSON["body"].get<string>();
    
    bool plaintext = draftJSON["plaintext"].get<bool>();
    bool multisend = perRecipientBodies.is_object();

    shared_ptr<Message> existing = nullptr;
    if (draftJSON.count("id")) {
        existing = store->find<Message>(Query().equal("id", draftJSON["id"].get<string>()));
    }
    Message draft = inflateClientDraftJSON(draftJSON, existing);
    
    logger->info("- Sending draft {}", draft.headerMessageId());

    if (account->usesSmarterMailAPI()) {
        if (multisend) throw SyncException("smartermail-multisend-unsupported", "Per-recipient customized sends are not supported by the native SmarterMail send operation. Disable tracking/customization and try again; no email was sent.", false);
        shared_ptr<Message> parent;
        const auto localParent = draft._data.find("replyToMessageId");
        if (localParent != draft._data.end() && localParent->is_string()) {
            parent = store->find<Message>(Query().equal("accountId", account->id()).equal("id", localParent->get<string>()));
        }
        const string parentMid = draft.forwardedHeaderMessageId().empty() ? draft.replyToHeaderMessageId() : draft.forwardedHeaderMessageId();
        if (!parent && !parentMid.empty()) parent = store->find<Message>(Query().equal("accountId", account->id()).equal("headerMessageId", parentMid));
        auto payload = SmarterMailCompose::payload(draft, account->emailAddress(), body, plaintext, parent.get());
        SmarterMailClient client(account);
        const string composeGuid = SmarterMailCompose::guid();
        json attachments = json::array(), names = json::array();
        set<string> stagedNames;
        for (auto & fileJSON : draft.files()) {
            File file(fileJSON);
            const string root = MailUtils::getEnvUTF8("CONFIG_DIR_PATH") + FS_PATH_SEP + "files";
            const string path = MailUtils::pathForFile(root, &file, false);
#ifdef _MSC_VER
            wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
            auto attachment = Attachment::attachmentWithContentsOfFile(AS_WIDE_MCSTR(convert.from_bytes(path)));
#else
            auto attachment = Attachment::attachmentWithContentsOfFile(AS_MCSTR(path));
#endif
            if (!attachment || !attachment->data()) throw SyncException("smartermail-missing-attachment", "An attachment is unavailable; no email was sent.", false);
            auto data = attachment->data();
            const string bytes((const char *)data->bytes(), data->length());
            const string type = file.contentType().empty() ? "application/octet-stream" : file.contentType();
            // Match the reference client: embed inline images as data URLs;
            // message-put's inline attachment flags are unreliable in Outlook.
            if (!plaintext && file.contentId().is_string() && type.rfind("image/", 0) == 0) {
                string cid = file.contentId().get<string>();
                if (cid.size() > 2 && cid.front() == '<' && cid.back() == '>') cid = cid.substr(1, cid.size() - 2);
                const string needle = "cid:" + cid;
                const string replacement = "data:" + type + ";base64," + MailUtils::toBase64(bytes.data(), bytes.size());
                size_t at = 0; bool embedded = false;
                while (!cid.empty() && (at = body.find(needle, at)) != string::npos) {
                    body.replace(at, needle.size(), replacement); at += replacement.size(); embedded = true;
                }
                if (embedded) continue;
            }
            const string filename = file.filename();
            if (!stagedNames.insert(filename).second) throw SyncException("smartermail-duplicate-attachment-name", "Rename attachments with identical filenames before sending; no email was sent.", false);
            client.uploadComposeAttachment(composeGuid, filename, type, bytes);
            attachments.push_back({{"filename", filename}, {"fileName", filename}, {"Filename", filename}, {"FileName", filename},
                {"attachmentName", filename}, {"AttachmentName", filename}, {"name", filename}, {"Name", filename},
                {"contentType", type}, {"ContentType", type}, {"size", bytes.size()}, {"Size", bytes.size()}});
            names.push_back(filename);
        }
        payload[plaintext ? "messagePlainText" : "messageHTML"] = body;
        if (!attachments.empty()) {
            payload["attachments"] = attachments; payload["attachmentNames"] = names; payload["attachmentGuid"] = composeGuid;
        }
        logger->info("-- Sending through SmarterMail message-put (reply={}, forward={}, attachments={})",
            payload.value("isReply", false), payload.value("isForward", false), attachments.size());
        client.sendMessage(payload);
        // The server owns the Sent copy and its IDs. Never IMAP-append a second
        // locally-built MIME copy or retry via SMTP after this point.
        if (draft.remoteUID() && draft.remoteUID() <= UINT32_MAX - 5) {
            try { client.remove(draft.remoteFolder().value("path", ""), {draft.remoteUID()}); }
            catch (const SyncException &) { logger->warn("SmarterMail sent the message; remote draft cleanup failed."); }
        }
        if (existing) store->remove(existing.get());
        MailUtils::wakeAllWorkers();
        return;
    }

    // find the sent folder: folder OR label
    auto sent = store->find<Folder>(Query().equal("accountId", account->id()).equal("role", "sent"));
    if (sent == nullptr) {
        sent = store->find<Label>(Query().equal("accountId", account->id()).equal("role", "sent"));
        if (sent == nullptr) {
            throw SyncException("no-sent-folder", "SummerMail doesn't know which folder to use for sent mail. Visit Preferences > Folders to assign a sent folder.", false);
        }
    }
    String * sentPath = AS_MCSTR(sent->path());
    logger->info("-- Identified `sent` folder: {}", sent->path());
    
    // build the MIME message
    MessageBuilder builder;
    if (multisend) {
        if (!perRecipientBodies.count("self")) {
            throw SyncException("no-self-body", "If `perRecipientBodies` is populated, you must provide a `self` entry.", false);
        }
        if (plaintext) {
            builder.setTextBody(AS_MCSTR(perRecipientBodies["self"].get<string>()));
        } else {
            builder.setHTMLBody(AS_MCSTR(perRecipientBodies["self"].get<string>()));
        }
    } else {
        if (plaintext) {
            builder.setTextBody(AS_MCSTR(body));
        } else {
            builder.setHTMLBody(AS_MCSTR(body));
        }
    }

    builder.header()->setSubject(AS_MCSTR(draft.subject()));
    builder.header()->setMessageID(AS_MCSTR(draft.headerMessageId()));
    builder.header()->setUserAgent(MCSTR("SummerMail"));
    builder.header()->setDate(time(0));
    
    ReplyHeaders::apply(builder.header(), draft, store, account->usesSmarterMailAPI(),
        [&](Message * parent) -> MessageHeader * {
            const string path = parent->remoteFolder().value("path", "");
            if (path.empty() || parent->remoteUID() == 0) return nullptr;
            if (account->usesSmarterMailAPI()) {
                SmarterMailClient client(account);
                auto detail = client.messageBody(path, parent->remoteUID());
                auto parse = [](const string & mime) {
                    return MessageParser::messageParserWithData(Data::dataWithBytes(mime.data(), (unsigned int)mime.size()))->header();
                };
                auto header = parse(detail.mime);
                // Match the reference project's raw-header fallback. Do this
                // at send time only when the parent's headers aren't verified.
                if (header->isMessageIDAutoGenerated() || !header->references() || header->references()->count() == 0) {
                    try {
                        const string raw = client.rawMessage(path, parent->remoteUID());
                        if (!raw.empty()) {
                            auto rawHeader = parse(raw);
                            if (!rawHeader->isMessageIDAutoGenerated()) header = rawHeader;
                        }
                    } catch (const SyncException &) {
                        // A real root message may have no References. A missing
                        // raw endpoint must not discard its valid JSON headers.
                        if (header->isMessageIDAutoGenerated()) throw;
                    }
                }
                return header;
            }
            ErrorCode fetchError = ErrorNone;
            auto rows = session->fetchMessagesByUID(AS_MCSTR(path), IMAPMessagesRequestKindHeaders,
                IndexSet::indexSetWithIndex(parent->remoteUID()), nullptr, &fetchError);
            if (fetchError != ErrorNone) throw SyncException(fetchError, "reply parent headers");
            if (!rows || rows->count() != 1) return nullptr;
            return ((IMAPMessage *)rows->objectAtIndex(0))->header();
        }, !account->usesMicrosoftGraph());
    if (draft.forwardedHeaderMessageId() != "") {
        builder.header()->setReferences(Array::arrayWithObject(AS_MCSTR(draft.forwardedHeaderMessageId())));
    }

    Array * to = Array::array();
    for (json & p : draft.to()) {
        to->addObject(MailUtils::addressFromContactJSON(p));
    }
    builder.header()->setTo(to);

    Array * cc = Array::array();
    for (json & p : draft.cc()) {
        cc->addObject(MailUtils::addressFromContactJSON(p));
    }
    builder.header()->setCc(cc);

    Array * bcc = Array::array();
    for (json & p : draft.bcc()) {
        bcc->addObject(MailUtils::addressFromContactJSON(p));
    }
    builder.header()->setBcc(bcc);

    Array * replyTo = Array::array();
    for (json & p : draft.replyTo()) {
        replyTo->addObject(MailUtils::addressFromContactJSON(p));
    }
    builder.header()->setReplyTo(replyTo);

    json & fromP = draft.from().at(0);
    builder.header()->setFrom(MailUtils::addressFromContactJSON(fromP));

    // Inject importance/priority headers for max client compatibility.
    // Front-end stores the canonical value under `hImportance` ("high" | "low" | "normal").
    if (draft._data.count("hImportance") && draft._data["hImportance"].is_string()) {
        string importance = draft._data["hImportance"].get<string>();
        if (importance == "high") {
            builder.header()->setExtraHeader(MCSTR("Importance"), MCSTR("high"));
            builder.header()->setExtraHeader(MCSTR("X-Priority"), MCSTR("1 (Highest)"));
            builder.header()->setExtraHeader(MCSTR("X-MSMail-Priority"), MCSTR("High"));
        } else if (importance == "low") {
            builder.header()->setExtraHeader(MCSTR("Importance"), MCSTR("low"));
            builder.header()->setExtraHeader(MCSTR("X-Priority"), MCSTR("5 (Lowest)"));
            builder.header()->setExtraHeader(MCSTR("X-MSMail-Priority"), MCSTR("Low"));
        }
    }

    for (json & fileJSON : draft.files()) {
        File file{fileJSON};
        string root = MailUtils::getEnvUTF8("CONFIG_DIR_PATH") + FS_PATH_SEP + "files";
        string path = MailUtils::pathForFile(root, &file, false);
        
#ifdef _MSC_VER
        wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
        Attachment * a = Attachment::attachmentWithContentsOfFile(AS_WIDE_MCSTR(convert.from_bytes(path)));
#else
        Attachment * a = Attachment::attachmentWithContentsOfFile(AS_MCSTR(path));
#endif

        if (file.contentId().is_string()) {
            a->setContentID(AS_MCSTR(file.contentId().get<string>()));
            a->setInlineAttachment(true);
            builder.addRelatedAttachment(a);
        } else {
            builder.addAttachment(a);
        }
    }

    // Save the message data / body we'll write to the sent folder
    Data * messageDataForSent = builder.data();

    if (account->usesMicrosoftGraph()) {
        if (multisend) {
            throw SyncException("graph-multisend-unsupported", "Per-recipient multisend is not yet supported for Microsoft Graph accounts.", false);
        }
        auto token = SharedXOAuth2TokenManager()->partsForAccount(account).accessToken;
        string mime((const char *)messageDataForSent->bytes(), messageDataForSent->length());
        string encoded = MailUtils::toBase64(mime.c_str(), mime.size());
        CURL * request = CreateMicrosoftGraphMimeRequest(
            MicrosoftGraphBaseURL(account) + "/sendMail", token, encoded);
        PerformRequest(request);
        CleanupCurlRequest(request);
        if (existing) store->remove(existing.get());
        return;
    }

    /*
    OK! If we've reached this point we're going to deliver the message. To do multisend,
    we need to hit the SMTP gateway more than once. If one request fails we stop and mark
    the task as failed, but keep track of who got the message.
    */

    SMTPSession smtp;
    SMTPProgress sprogress;
    MailUtils::configureSessionForAccount(smtp, account);
    string succeeded;

    if (multisend) {
        logger->info("-- Sending customized message bodies to each recipient:");

        for (json::iterator it = perRecipientBodies.begin(); it != perRecipientBodies.end(); ++it) {
            if (it.key() == "self") {
                continue;
            }
            
            logger->info("--- Sending to {}", it.key());
            if (plaintext) {
                builder.setTextBody(AS_MCSTR(it.value().get<string>()));
            } else {
                builder.setHTMLBody(AS_MCSTR(it.value().get<string>()));
            }
            Address * to = Address::addressWithMailbox(AS_MCSTR(it.key()));
            Data * messageData = builder.data();
            smtp.sendMessage(builder.header()->from(), Array::arrayWithObject(to), messageData, &sprogress, &err);
            if (err != ErrorNone) {
                break;
            }
            succeeded += "\n - " + it.key();
        }

    } else {
        logger->info("-- Sending a single message body to all recipients:");
        smtp.sendMessage(messageDataForSent, &sprogress, &err);
    }
    
    if (err != ErrorNone) {
        int e = smtp.lastLibetpanError();
        string es = LibEtPanCodeToTypeMap.count(e) ? LibEtPanCodeToTypeMap[e] : to_string(e);
        logger->info("-X An SMTP error occurred: {} LibEtPan code: {}", ErrorCodeToTypeMap[err], es);
        if (succeeded.size() > 0) {
            throw SyncException("send-partially-failed", ErrorCodeToTypeMap[err] + ":::" + succeeded, false);
        } else {
            throw SyncException("send-failed", ErrorCodeToTypeMap[err], false);
        }
    }

    IMAPSession smarterMailSentSession;
    IMAPSession * sentSession = session;
    const bool smarterMailSend = account->usesSmarterMailAPI();
    if (smarterMailSend) {
        SmarterMailClient client(account);
        if (draft.remoteUID() != 0 && draft.remoteUID() <= UINT32_MAX - 5) {
            try { client.remove(draft.remoteFolder().value("path", ""), {draft.remoteUID()}); }
            catch (SyncException & ex) { logger->warn("Could not remove the SmarterMail draft after send: {}", ex.toJSON().dump()); }
        }

        // SmarterMail's REST MIME-import routes are compatibility probes and are
        // not implemented by the supported v17 server. The reference client saves
        // the already-delivered RFC822 message with a narrow IMAP APPEND instead.
        // Mail listing and body sync remain on the native API.
        MailUtils::configureSessionForAccount(smarterMailSentSession, account);
        smarterMailSentSession.connect(&err);
        if (err != ErrorNone) {
            logger->error("-X SmarterMail accepted the SMTP send, but IMAP could not connect to archive the Sent copy: {}",
                          ErrorCodeToTypeMap[err]);
            store->remove(&draft);
            return;
        }
        sentSession = &smarterMailSentSession;
        err = ErrorNone;
    }
    
    /* 
     Sending complete! First, delete the draft from the server so the user knows it has been sent
     and we don't re-sync it to the app after we delete it below.
     */
    if (!smarterMailSend && draft.remoteUID() != 0) {
        auto uids = IndexSet::indexSetWithIndex(draft.remoteUID());
        String * path = AS_MCSTR(draft.remoteFolder()["path"].get<string>());
        
        logger->info("-- Deleting remote draft with UID {}", draft.remoteUID());
        _removeMessagesResilient(session, store, account->id(), path, uids);
    }

     /* Next, scan the sent folder for the message(s) we just sent through the SMTP
     gateway and clean them up. Some mail servers automatically place messages in the sent
     folder, others don't.
     */
    uint32_t sentFolderMessageUID = 0;
    if (!smarterMailSend) {
        // grab the last few items in the sent folder... we know we don't need more than 10
        // because multisend is capped.
        int tries = 0;
        int delay[] = {0, 1, 1, 2, 2};
        IndexSet * uids = IndexSet::indexSet();
        
        while (tries < 4 && uids->count() == 0) {
            if (delay[tries]) {
                logger->info("-- No messages found. Sleeping {} to wait for sent folder to settle...", delay[tries]);
				std::this_thread::sleep_for(std::chrono::seconds(delay[tries]));
            }
            tries ++;
            sentSession->findUIDsOfRecentHeaderMessageID(sentPath, AS_MCSTR(draft.headerMessageId()), uids);
        }
    
        if (multisend && (uids->count() > 0)) {
            // If we sent separate messages to each recipient, we end up with a bunch of sent
            // messages. Delete all of them since they contain the targeted bodies with link/open tracking.
            logger->info("-- Deleting {} messages added to {} by the SMTP gateway.", uids->count(), sentPath->UTF8Characters());
            _removeMessagesResilient(session, store, account->id(), sentPath, uids);
            
            // In Gmail, moving the messages from Sent -> Trash and expunging them just places them in All Mail
            // for some reason. Deleting them AGAIN from All Mail works properly, so we do that here.
            auto all = store->find<Folder>(Query().equal("accountId", account->id()).equal("role", "all"));
            if (all != nullptr) {
                uids->removeAllIndexes();
                sentSession->findUIDsOfRecentHeaderMessageID(AS_MCSTR(all->path()), AS_MCSTR(draft.headerMessageId()), uids);
                if (uids->count() > 0) {
                    logger->info("-- Deleting {} messages just moved to {} by the SMTP gateway.", uids->count(), all->path());
                    _removeMessagesResilient(session, store, account->id(), AS_MCSTR(all->path()), uids);
                }
            }

        } else if (!multisend && (uids->count() == 1)) {
            // If we find a single message in the sent folder, we'll move forward with that one.
            sentFolderMessageUID = (uint32_t)uids->allRanges()[0].location;
            logger->info("-- Found a message added to the sent folder by the SMTP gateway (UID {})", sentFolderMessageUID);
            
        } else {
            logger->info("-- No messages matching the message-id were found in the Sent folder.", uids->count());
        }
        
        if (err != ErrorNone) {
            logger->error("-X IMAP Error: {}. This may result in duplicate messages in the Sent folder.", ErrorCodeToTypeMap[err]);
            err = ErrorNone;
        }
    }

    if (sentFolderMessageUID == 0) {
        // Manually place a single message in the sent folder
        IMAPProgress iprogress;
        logger->info("-- Placing a new message with `self` body in the sent folder.");
        sentSession->appendMessage(sentPath, messageDataForSent, MessageFlagSeen, &iprogress, &sentFolderMessageUID, &err);
        if (err != ErrorNone) {
            logger->error("-X IMAP Error: {}. Could not place a message into the Sent folder. This means no metadata will be attached!", ErrorCodeToTypeMap[err]);
            err = ErrorNone;
        }

        // If the user is on Gmail and the thread had labels, apply those same
        // labels to the new sent message. Otherwise the thread moves /only/ to
        // the sent folder.
        if (sentSession->storedCapabilities()->containsIndex(IMAPCapabilityGmail)) {
            if (draft.threadId() != "") {
                auto thread = store->find<Thread>(Query().equal("id", draft.threadId()));
                if (thread) {
                    Array * xgmValues = Array::array();
                    for (auto & l : thread->labels()) {
                        string role = l["role"].get<string>();
                        if (role == "inbox" || role == "sent" || role == "drafts") { continue; }
                        string xgm = _xgmKeyForLabel(l);
                        logger->info("-- Will add label to new message: {}", xgm);
                        xgmValues->addObject(AS_MCSTR(xgm));
                    }
                    sentSession->storeLabelsByUID(sentPath, IndexSet::indexSetWithIndex(sentFolderMessageUID), IMAPStoreFlagsRequestKindAdd, xgmValues, &err);
                    if (err != ErrorNone) {
                        logger->error("-X IMAP Error: {}. Could not add labels to new message in sent folder. This means the thread may disappear from the inbox.", ErrorCodeToTypeMap[err]);
                        err = ErrorNone;
                    }
                }
            }
        }
    }

    if (sentFolderMessageUID == 0) {
        // If we still don't have a message in the sent folder, there's nothing we can do.
        // Delete the draft and exit.
        store->remove(&draft);
        return;
    }

    /*
     Finally, pull down the message we created to get it's labels, thread ID, etc. and
     associate our metadata with it.
     
     Note: Yes, it's a bit weird that we sync up a message to the sent folder and then
     immediately pull it's attributes, but we want to get the Thread ID on Gmail, etc.
     We don't pull down the entire message body.
     */

    MailProcessor processor{account, store};
    shared_ptr<Message> localMessage = nullptr;
    IMAPMessage * remoteMessage = nullptr;
    
    logger->info("-- Syncing sent message (UID {}) to the local mail store", sentFolderMessageUID);
    IMAPMessagesRequestKind kind = (IMAPMessagesRequestKind)(IMAPMessagesRequestKindHeaders | IMAPMessagesRequestKindFlags);
    if (sentSession->storedCapabilities()->containsIndex(IMAPCapabilityGmail)) {
        kind = (IMAPMessagesRequestKind)(kind | IMAPMessagesRequestKindGmailLabels | IMAPMessagesRequestKindGmailThreadID | IMAPMessagesRequestKindGmailMessageID);
    }
    
    // Important: Courier (and maybe other IMAP servers) won't show us new messages we've created
    // in the folder unless we re-select the folder. (I think they're treating UIDs like sequence
    // numbers?). We must re-select the sent folder to pull down the message we created.
    sentSession->select(sentPath, &err);

    time_t syncDataTimestamp = time(0);
    IndexSet * uids = IndexSet::indexSetWithIndex(sentFolderMessageUID);
    Array * remote = sentSession->fetchMessagesByUID(sentPath, kind, uids, nullptr, &err);

    // Delete the draft. We do this as close as possible to when we write the message in
    // so there isn't any flicker in the client, but before error checking because we always
    // want it to always disppear since sending succeeded.
    store->remove(&draft);

    if (err != ErrorNone) {
        logger->error("-X Error: {} occurred syncing the sent message to the local mail store. Metadata will not be attached.", ErrorCodeToTypeMap[err]);
        return;
    }
    if (remote->count() == 0) {
        logger->error("-X Error: No messages were returned. Metadata will not be attached!");
        return;
    }

    MessageParser * messageParser = MessageParser::messageParserWithData(messageDataForSent);
    remoteMessage = (IMAPMessage *)(remote->lastObject());
    localMessage = processor.insertFallbackToUpdateMessage(remoteMessage, *sent, syncDataTimestamp);
    if (localMessage == nullptr) {
        logger->error("-X Error: processor.insert did not return a message.");
        return;
    }

    processor.retrievedMessageBody(localMessage.get(), messageParser);
    
    logger->info("-- Synced sent message (Sent UID {} = Local ID {})", sentFolderMessageUID, localMessage->id());
    
    // retrieve the new message and queue metadata tasks on it.
    // Metadata entries whose pluginId starts with "thread:" are promoted to the
    // thread (with the prefix stripped) rather than attached to the message.
    // This lets plugins declare thread-level intent at draft-write time.
    static const string THREAD_PREFIX = "thread:";
    for (const auto & m : draft.metadata()) {
        auto pluginId = m["pluginId"].get<string>();

        if (pluginId.substr(0, THREAD_PREFIX.size()) == THREAD_PREFIX) {
            string actualPluginId = pluginId.substr(THREAD_PREFIX.size());
            string threadId = localMessage->threadId();
            logger->info("-- Queueing task to attach {} draft metadata to thread {} (thread: prefix).", actualPluginId, threadId);
            Task mTask{"SyncbackMetadataTask", account->id(), {
                {"modelId", threadId},
                {"modelClassName", "thread"},
                {"pluginId", actualPluginId},
                {"value", m["value"]},
            }};
            performLocal(&mTask); // will call save
        } else {
            logger->info("-- Queueing task to attach {} draft metadata to new message.", pluginId);
            Task mTask{"SyncbackMetadataTask", account->id(), {
                {"modelId", localMessage->id()},
                {"modelClassName", "message"},
                {"modelHeaderMessageId", localMessage->headerMessageId()},
                {"pluginId", pluginId},
                {"value", m["value"]},
            }};
            performLocal(&mTask); // will call save
        }
    }
}

void TaskProcessor::performRemoteSendFeatureUsageEvent(Task * task) {
    if (Identity::GetGlobal() == nullptr) {
        logger->info("Skipped metadata sync, not logged in.");
        return;
    }
    const auto feature = task->data()["feature"].get<string>();
    json payload = {
        {"feature", feature}
    };

    logger->info("Incrementing usage of feature: {}", feature);
    auto result = PerformIdentityRequest("/api/feature_usage_event", "POST", payload);
    logger->info("Incrementing usage of feature succeeded: {}", result.dump());
}

void TaskProcessor::performLocalChangeRoleMapping(Task * task) {
    const auto path = task->data()["path"].get<string>();
    const auto role = task->data()["role"].get<string>();
    
    {
        MailStoreTransaction transaction{store, "performLocalChangeRoleMapping"};
        auto query = Query().equal("accountId", task->accountId()).equal("path", path);
        shared_ptr<Folder> category = store->find<Folder>(query);
        if (category == nullptr) {
            category = store->find<Label>(query);
        }
        if (category == nullptr) {
            throw SyncException("no-matching-folder", "", false);
        }
        
        // find any category with the role already and clear it
        auto existingQuery = Query().equal("accountId", task->accountId()).equal("role", role);
        shared_ptr<Folder> existing = store->find<Folder>(existingQuery);
        if (existing == nullptr) {
            existing = store->find<Label>(existingQuery);
        }
        if (existing != nullptr) {
            existing->setRole("");
            store->save(existing.get());
        }
    
        // save role onto the new category
        category->setRole(role);
        store->save(category.get());
        
        transaction.commit();
    }
}

void TaskProcessor::performRemoteExpungeAllInFolder(Task * task) {
    AutoreleasePool pool;
    ErrorCode err = ErrorNone;
    const auto path = task->data()["folder"]["path"].get<string>();
    const auto id = task->data()["folder"]["id"].get<string>();

    if (account->usesSmarterMailAPI()) {
        auto messages = store->findAll<Message>(Query().equal("accountId", task->accountId()).equal("remoteFolderId", id));
        SmarterMailClient client(account);
        for (auto block : MailUtils::chunksOfVector(messages, 200)) {
            vector<uint32_t> uids;
            for (auto & message : block) if (message->remoteUID() <= UINT32_MAX - 5) uids.push_back(message->remoteUID());
            if (!uids.empty()) client.remove(path, uids, false);
            MailStoreTransaction transaction(store, "expungeSmarterMailFolder");
            for (auto & message : block) store->remove(message.get());
            transaction.commit();
        }
        return;
    }

    if (account->usesMicrosoftGraph()) {
        auto messages = store->findAll<Message>(Query().equal("accountId", task->accountId()).equal("remoteFolderId", id));
        auto token = SharedXOAuth2TokenManager()->partsForAccount(account).accessToken;
        for (auto & message : messages) {
            if (!message->graphId().empty()) {
                string url = MicrosoftGraphBaseURL(account) + "/messages/" + taskGraphUrlEncode(message->graphId());
                PerformJSONRequest(CreateMicrosoftGraphRequest(url, "DELETE", token));
            }
            store->remove(message.get());
        }
        return;
    }

    IndexSet set;
    set.addRange(RangeMake(1, UINT64_MAX));
    session->storeFlagsByUID(AS_MCSTR(path), &set, IMAPStoreFlagsRequestKindAdd, MessageFlagDeleted, &err);
    if (err != ErrorNone) {
        throw SyncException(err, "storeFlagsByUID");
    }
    session->expunge(AS_MCSTR(path), &err);
    if (err != ErrorNone) {
        throw SyncException(err, "expunge");
    }
    logger->info("-- Expunged {}", path);
    
    // delete all the local messages in the folder. We do this in performRemote
    // because we don't want to block in performLocal for this long. We also pause
    // as we go to allow the app to recover from the mass deletions.
    auto all = store->findAll<Message>(Query().equal("accountId", task->accountId()).equal("remoteFolderId", id));
    for (auto block : MailUtils::chunksOfVector(all, 100)) {
        {
            MailStoreTransaction t {store};
            for (auto msg : block) {
                store->remove(msg.get());
            }
            t.commit();
        }
        logger->info("-- Deleted {} local messages", block.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

void TaskProcessor::performRemoteGetMessageRFC2822(Task * task) {
    AutoreleasePool pool;
    IMAPProgress cb;
    ErrorCode err = ErrorNone;
    const auto id = task->data()["messageId"].get<string>();
    const auto filepath = task->data()["filepath"].get<string>();
    
    auto msg = store->find<Message>(Query().equal("id", id));
    if (msg == nullptr) {
        throw SyncException("not-found", "Message not found for RFC2822 fetch", false);
    }

    Data * data = nullptr;
    if (account->usesSmarterMailAPI()) {
        string raw = SmarterMailClient(account).rawMessage(msg->remoteFolder().value("path", ""), msg->remoteUID());
        data = Data::dataWithBytes(raw.data(), (unsigned int)raw.size());
    } else if (account->usesMicrosoftGraph()) {
        data = fetchMicrosoftGraphMIME(account, msg.get());
    } else {
        data = session->fetchMessageByUID(AS_MCSTR(msg->remoteFolder()["path"].get<string>()), msg->remoteUID(), &cb, &err);
    }
    if (err != ErrorNone) {
        logger->error("Unable to fetch rfc2822 for message (UID {}). Error {}", msg->remoteUID(), ErrorCodeToTypeMap[err]);
        throw SyncException(err, "performRemoteGetMessageRFC2822");
    }
    if (data == nullptr) {
        logger->error("fetchMessageByUID returned null data for message (UID {})", msg->remoteUID());
        throw SyncException(ErrorFetch, "performRemoteGetMessageRFC2822 - null data");
    }
#ifdef _MSC_VER
    wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
    data->writeToFile(AS_WIDE_MCSTR(convert.from_bytes(filepath)));
#else
    data->writeToFile(AS_MCSTR(filepath));
#endif
    setFileModificationTime(filepath, msg->date());
}

std::string TaskProcessor::sanitizeEmlFilename(const std::string & subject, time_t date, int index) {
    // Start with the subject, or "untitled" if empty
    std::string safe = subject.empty() ? "untitled" : subject;

    // Truncate to 80 characters (respecting multi-byte boundaries isn't critical
    // since illegal chars are replaced anyway, and truncation mid-char produces
    // a replacement underscore at worst)
    if (safe.size() > 80) {
        safe.resize(80);
    }

    // Replace filesystem-illegal characters and control characters with '_'
    for (size_t i = 0; i < safe.size(); i++) {
        unsigned char c = static_cast<unsigned char>(safe[i]);
        if (c <= 0x1f || c == 0x7f ||
            safe[i] == '/' || safe[i] == '?' || safe[i] == '<' || safe[i] == '>' ||
            safe[i] == '\\' || safe[i] == ':' || safe[i] == '*' || safe[i] == '|' || safe[i] == '"') {
            safe[i] = '_';
        }
    }

    // Strip trailing dots and spaces (Windows requirement)
    while (!safe.empty() && (safe.back() == '.' || safe.back() == ' ')) {
        safe.pop_back();
    }
    if (safe.empty()) {
        safe = "untitled";
    }

    // Format date as YYYY-MM-DD in UTC
    char dateBuf[16];
    struct tm utc;
#ifdef _MSC_VER
    gmtime_s(&utc, &date);
#else
    gmtime_r(&date, &utc);
#endif
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &utc);

    // Build filename: {index} - {subject} - {date}.eml
    std::ostringstream oss;
    oss << std::setw(5) << std::setfill('0') << index
        << " - " << safe << " - " << dateBuf << ".eml";

    std::string filename = oss.str();

#ifdef _MSC_VER
    // Clamp to 200 characters for Windows MAX_PATH safety
    if (filename.size() > 200) {
        // Rebuild with a truncated subject to fit
        size_t overhead = 5 + 3 + 3 + strlen(dateBuf) + 4; // index + " - " + " - " + date + ".eml"
        size_t maxSubject = 200 - overhead;
        if (safe.size() > maxSubject) {
            safe.resize(maxSubject);
            // Re-strip trailing dots/spaces after truncation
            while (!safe.empty() && (safe.back() == '.' || safe.back() == ' ')) {
                safe.pop_back();
            }
            if (safe.empty()) safe = "untitled";
        }
        oss.str("");
        oss.clear();
        oss << std::setw(5) << std::setfill('0') << index
            << " - " << safe << " - " << dateBuf << ".eml";
        filename = oss.str();
    }
#endif

    return filename;
}

void TaskProcessor::performRemoteGetManyRFC2822(Task * task) {
    const auto folderId = task->data()["folderId"].get<string>();
    const auto folderPath = task->data()["folderPath"].get<string>();
    const auto outputDir = task->data()["outputDir"].get<string>();

    // Get total count without loading all message objects into memory
    auto countQuery = Query().equal("accountId", task->accountId()).equal("remoteFolderId", folderId);
    int total = store->count<Message>(countQuery);

    // Initialize or resume progress
    json progress;
    progress["total"] = total;
    progress["exported"] = 0;
    progress["failed"] = 0;
    progress["errors"] = json::array();

    // If resuming, carry forward previous progress
    uint32_t cursorUID = 0;
    int globalIndex = 0;
    if (task->data().count("progress")) {
        auto & prev = task->data()["progress"];
        if (prev.count("exported")) {
            progress["exported"] = prev["exported"];
            globalIndex = prev["exported"].get<int>();
        }
        if (prev.count("failed")) {
            progress["failed"] = prev["failed"];
        }
        if (prev.count("errors")) {
            progress["errors"] = prev["errors"];
        }
        if (prev.count("lastUID")) {
            cursorUID = prev["lastUID"].get<uint32_t>();
        }
    }

    int exported = progress["exported"].get<int>();
    int failed = progress["failed"].get<int>();
    const int chunkSize = 50;

    // Paginate through messages ordered by remoteUID ascending, using a cursor
    // to avoid skipping/duplicating messages if the folder changes during export.
    // This uses the MessageUIDScanIndex (accountId, remoteFolderId, remoteUID).
    while (true) {
        AutoreleasePool pool;

        auto chunkQuery = Query()
            .equal("accountId", task->accountId())
            .equal("remoteFolderId", folderId)
            .gt("remoteUID", (double)cursorUID)
            .orderBy("remoteUID", "ASC")
            .limit(chunkSize);
        auto messages = store->findAll<Message>(chunkQuery);

        if (messages.empty()) {
            break;
        }

        for (auto & msg : messages) {
            std::string filename = sanitizeEmlFilename(msg->subject(), msg->date(), globalIndex);
            std::string filepath = outputDir + FS_PATH_SEP + filename;

            IMAPProgress cb;
            ErrorCode err = ErrorNone;

            try {
                Data * data = nullptr;
                if (account->usesSmarterMailAPI()) {
                    string raw = SmarterMailClient(account).rawMessage(folderPath, msg->remoteUID());
                    data = Data::dataWithBytes(raw.data(), (unsigned int)raw.size());
                } else if (account->usesMicrosoftGraph()) {
                    data = fetchMicrosoftGraphMIME(account, msg.get());
                } else {
                    data = session->fetchMessageByUID(AS_MCSTR(folderPath), msg->remoteUID(), &cb, &err);
                }

                if (err != ErrorNone) {
                    throw SyncException(err, "GetManyRFC2822 fetch");
                }
                if (data == nullptr) {
                    throw SyncException(ErrorFetch, "GetManyRFC2822 - null data");
                }

#ifdef _MSC_VER
                wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
                data->writeToFile(AS_WIDE_MCSTR(convert.from_bytes(filepath)));
#else
                data->writeToFile(AS_MCSTR(filepath));
#endif
                setFileModificationTime(filepath, msg->date());
                exported++;
            } catch (SyncException & ex) {
                logger->error("GetManyRFC2822: failed to export message {} (UID {}): {}",
                    msg->id(), msg->remoteUID(), ex.toJSON().dump());
                failed++;
                json errEntry;
                errEntry["messageId"] = msg->id();
                errEntry["subject"] = msg->subject();
                errEntry["error"] = ex.toJSON()["error"];
                progress["errors"].push_back(errEntry);
            }

            cursorUID = msg->remoteUID();
            globalIndex++;
        }

        // After each chunk, persist progress and check for a cancel request in
        // a single write transaction.
        //
        // Electron sets should_cancel on this same Task row from the main
        // thread (TaskProcessor::cancel), using a different MailStore
        // connection. Previously this code saved our in-memory task copy — in
        // which should_cancel is false — and only then re-read the row, so the
        // progress save clobbered a concurrently-set cancel flag and the
        // re-read almost never saw it: once the export had started it could not
        // be cancelled. Re-reading and saving inside one BEGIN IMMEDIATE
        // transaction serializes against that cancel write, so a request is
        // either already visible here or ordered strictly after our commit and
        // seen on the next chunk — never lost.
        progress["exported"] = exported;
        progress["failed"] = failed;
        progress["lastUID"] = cursorUID;

        bool cancelled = false;
        {
            MailStoreTransaction transaction{store, "GetManyRFC2822 progress"};
            auto refreshed = store->find<Task>(Query().equal("id", task->id()));
            if (refreshed != nullptr && refreshed->shouldCancel()) {
                cancelled = true;
                progress["cancelled"] = true;
                // Carry the flag onto our in-memory copy so this save preserves
                // it and performRemote marks the task cancelled, not complete.
                task->setShouldCancel();
            }
            task->data()["progress"] = progress;
            store->save(task);
            transaction.commit();
        }
        if (cancelled) {
            logger->info("GetManyRFC2822: cancelled after exporting {} of {} messages", exported, total);
            return;
        }

        // Sleep to let sync and other tasks breathe
        if ((int)messages.size() == chunkSize) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // Write final result
    json result;
    result["total"] = total;
    result["exported"] = exported;
    result["failed"] = failed;
    result["outputDir"] = outputDir;
    result["errors"] = progress["errors"];
    task->data()["result"] = result;
    store->save(task);

    logger->info("GetManyRFC2822: completed. Exported {} of {} messages ({} failed)",
        exported, total, failed);
}

void TaskProcessor::performRemoteCrossAccountMoveFolder(Task * task) {
    if (!task->data().count("phase") || !task->data()["phase"].is_string()) {
        throw SyncException("missing-json", "Cross-account transfer is missing a phase", false);
    }

    const string phase = task->data()["phase"].get<string>();
    if (phase == "prepare") {
        prepareCrossAccountMoveFolder(task);
    } else if (phase == "import") {
        importCrossAccountMoveFolder(task);
    } else {
        throw SyncException("invalid-json", "Unknown cross-account transfer phase", false);
    }
}

void TaskProcessor::prepareCrossAccountMoveFolder(Task * task) {
    json & data = task->data();
    if (!data.count("threadIds") || !data["threadIds"].is_array() ||
        !data.count("stagingDirectory") || !data["stagingDirectory"].is_string()) {
        throw SyncException("missing-json", "Cross-account prepare requires threadIds and stagingDirectory", false);
    }

    const string stagingDirectory = data["stagingDirectory"].get<string>();
    auto messages = inflateMessages(data).messages;
    if (messages.empty()) {
        throw SyncException("not-found", "No source messages were found for the selected conversations", false);
    }

    json result = data.count("result") && data["result"].is_object()
        ? data["result"]
        : json::object();
    if (!result.count("files") || !result["files"].is_array()) {
        result["files"] = json::array();
    }

    set<string> preparedMessageIds;
    for (auto & entry : result["files"]) {
        if (entry.count("messageId") && entry["messageId"].is_string()) {
            preparedMessageIds.insert(entry["messageId"].get<string>());
        }
    }

    int index = (int)result["files"].size();
    for (auto & msg : messages) {
        if (msg->isDraft() || msg->isDeletionPlaceholder() || msg->remoteUID() == 0) {
            continue;
        }
        if (preparedMessageIds.count(msg->id())) {
            continue;
        }

        AutoreleasePool pool;
        IMAPProgress progress;
        ErrorCode err = ErrorNone;
        Data * raw = session->fetchMessageByUID(
            AS_MCSTR(msg->remoteFolder()["path"].get<string>()), msg->remoteUID(), &progress, &err);
        if (err != ErrorNone) {
            throw SyncException(err, "Cross-account source RFC822 fetch");
        }
        if (raw == nullptr) {
            throw SyncException(ErrorFetch, "Cross-account source RFC822 fetch returned no data");
        }

        const string filepath = stagingDirectory + FS_PATH_SEP +
            to_string(index++) + "-" + msg->id() + ".eml";
#ifdef _MSC_VER
        wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
        ErrorCode writeErr = raw->writeToFile(AS_WIDE_MCSTR(convert.from_bytes(filepath)));
#else
        ErrorCode writeErr = raw->writeToFile(AS_MCSTR(filepath));
#endif
        if (writeErr != ErrorNone) {
            throw SyncException(writeErr, "Cross-account staging file write");
        }
        setFileModificationTime(filepath, msg->date());

        json entry;
        entry["filepath"] = filepath;
        entry["messageId"] = msg->id();
        entry["headerMessageId"] = msg->headerMessageId();
        entry["date"] = msg->date();
        entry["unread"] = msg->isUnread();
        entry["starred"] = msg->isStarred();
        result["files"].push_back(entry);
        result["completed"] = result["files"].size();
        data["result"] = result;
        store->save(task);
    }

    result["total"] = result["files"].size();
    result["completed"] = result["files"].size();
    data["result"] = result;
    if (result["files"].empty()) {
        throw SyncException("not-found", "The selected conversations contain no transferable messages", false);
    }
    store->save(task);
    logger->info("Cross-account prepare staged {} messages", result["files"].size());
}

void TaskProcessor::importCrossAccountMoveFolder(Task * task) {
    json & data = task->data();
    if (!data.count("files") || !data["files"].is_array() ||
        !data.count("targetFolder") || !data["targetFolder"].is_object()) {
        throw SyncException("missing-json", "Cross-account import requires files and targetFolder", false);
    }

    json & targetFolder = data["targetFolder"];
    if (!targetFolder.count("aid") || targetFolder["aid"].get<string>() != task->accountId() ||
        !targetFolder.count("path") || !targetFolder["path"].is_string()) {
        throw SyncException("bad-accountid", "Cross-account destination folder does not belong to this account", false);
    }
    const string targetPathString = targetFolder["path"].get<string>();
    String * targetPath = AS_MCSTR(targetPathString);

    json result = data.count("result") && data["result"].is_object()
        ? data["result"]
        : json::object();
    if (!result.count("appendedMessageIds") || !result["appendedMessageIds"].is_array()) {
        result["appendedMessageIds"] = json::array();
    }
    if (!result.count("skippedMessageIds") || !result["skippedMessageIds"].is_array()) {
        result["skippedMessageIds"] = json::array();
    }

    set<string> completed;
    for (auto & id : result["appendedMessageIds"]) completed.insert(id.get<string>());
    for (auto & id : result["skippedMessageIds"]) completed.insert(id.get<string>());

    for (auto & file : data["files"]) {
        const string messageId = file["messageId"].get<string>();
        if (completed.count(messageId)) continue;

        AutoreleasePool pool;
        const string filepath = file["filepath"].get<string>();
#ifdef _MSC_VER
        wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
        Data * raw = Data::dataWithContentsOfFile(AS_WIDE_MCSTR(convert.from_bytes(filepath)));
#else
        Data * raw = Data::dataWithContentsOfFile(AS_MCSTR(filepath));
#endif
        if (raw == nullptr) {
            throw SyncException("not-found", "A staged cross-account message file is missing", false);
        }

        bool alreadyPresent = false;
        const string headerMessageId = file.count("headerMessageId")
            ? file["headerMessageId"].get<string>()
            : "";
        if (!headerMessageId.empty() && headerMessageId != "no-header-message-id") {
            ErrorCode searchErr = ErrorNone;
            IMAPSearchExpression * expr = IMAPSearchExpression::searchHeader(
                MCSTR("Message-ID"), AS_MCSTR(headerMessageId));
            IndexSet * existing = session->search(targetPath, expr, &searchErr);
            if (searchErr == ErrorNone && existing != nullptr && existing->count() > 0) {
                alreadyPresent = true;
            } else if (searchErr != ErrorNone) {
                logger->warn("Cross-account Message-ID deduplication search failed: {}",
                    ErrorCodeToTypeMap[searchErr]);
            }
        }

        if (alreadyPresent) {
            result["skippedMessageIds"].push_back(messageId);
        } else {
            MessageFlag flags = MessageFlagNone;
            if (file.count("unread") && !file["unread"].get<bool>()) {
                flags = (MessageFlag)(flags | MessageFlagSeen);
            }
            if (file.count("starred") && file["starred"].get<bool>()) {
                flags = (MessageFlag)(flags | MessageFlagFlagged);
            }
            const time_t messageDate = file.count("date")
                ? (time_t)file["date"].get<long long>()
                : (time_t)-1;
            IMAPProgress progress;
            uint32_t createdUID = 0;
            ErrorCode appendErr = ErrorNone;
            session->appendMessageWithCustomFlagsAndDate(
                targetPath, raw, flags, nullptr, messageDate, &progress, &createdUID, &appendErr);
            if (appendErr != ErrorNone) {
                throw SyncException(appendErr, "Cross-account IMAP APPEND");
            }
            result["appendedMessageIds"].push_back(messageId);
        }

        completed.insert(messageId);
        result["total"] = data["files"].size();
        result["completed"] = completed.size();
        data["result"] = result;
        // Persist after every APPEND so a worker restart can resume without
        // duplicating messages already committed remotely.
        store->save(task);
    }

    logger->info("Cross-account import completed {} of {} messages",
        completed.size(), data["files"].size());
}

void TaskProcessor::performRemoteSendRSVP(Task * task) {
    AutoreleasePool pool;
    ErrorCode err = ErrorNone;

    // Validate required fields exist
    if (!task->data().count("ics") || !task->data().count("subject") || !task->data().count("to")) {
        throw SyncException("missing-json", "Missing required fields: ics, subject, or to", false);
    }

    // Load task data
    string ics = task->data()["ics"].get<string>();
    string subject = task->data()["subject"].get<string>();
    string organizer = task->data()["to"].get<string>();
    string icsRSVPStatus = task->data().count("icsRSVPStatus")
        ? task->data()["icsRSVPStatus"].get<string>()
        : "ACCEPTED";

    // =========================================================================
    // RFC 5546/6047 Validation
    // =========================================================================

    // Validation 1: Check ICS contains METHOD:REPLY (RFC 5546 requirement)
    if (ics.find("METHOD:REPLY") == string::npos) {
        throw SyncException("invalid-ics", "ICS data must contain METHOD:REPLY for an RSVP response", false);
    }

    // Parse the ICS to validate and extract event information
    ICalendar cal(ics);

    if (cal.Events.empty()) {
        throw SyncException("invalid-ics", "ICS data does not contain any VEVENT components", false);
    }

    ICalendarEvent* event = cal.Events.front();

    // Validation 2: UID is required (RFC 5546 Section 3.2.3 - MUST match original REQUEST)
    if (event->UID.empty()) {
        throw SyncException("invalid-ics",
            "ICS REPLY must contain UID property matching the original invitation", false);
    }

    // Validation 3: DTSTAMP is required (RFC 5546 Section 3.2.3)
    if (event->DtStamp.IsEmpty()) {
        throw SyncException("invalid-ics",
            "ICS REPLY must contain DTSTAMP property", false);
    }

    // Validation 4: ORGANIZER is required (RFC 5546 Section 3.2.3)
    if (event->Organizer.empty()) {
        throw SyncException("invalid-ics",
            "ICS REPLY must contain ORGANIZER property", false);
    }

    // Validation 5: REPLY must contain exactly one ATTENDEE (RFC 5546 Section 3.2.3)
    if (event->Attendees.size() != 1) {
        throw SyncException("invalid-ics",
            "ICS REPLY must contain exactly one ATTENDEE (the replying user), found " + to_string(event->Attendees.size()), false);
    }

    // Validation 6: Check ATTENDEE has valid PARTSTAT (RFC 5545 Section 3.2.12)
    bool hasValidPartstat = (ics.find("PARTSTAT=ACCEPTED") != string::npos ||
                             ics.find("PARTSTAT=DECLINED") != string::npos ||
                             ics.find("PARTSTAT=TENTATIVE") != string::npos);
    if (!hasValidPartstat) {
        throw SyncException("invalid-ics",
            "ATTENDEE must have valid PARTSTAT parameter (ACCEPTED, DECLINED, or TENTATIVE)", false);
    }

    // Extract attendee email for From address validation
    // The ICalendar library returns attendee as "Name <email>" or just "email"
    string attendeeInfo = event->Attendees.front();
    string attendeeEmail;
    size_t emailStart = attendeeInfo.find('<');
    size_t emailEnd = attendeeInfo.find('>');
    if (emailStart != string::npos && emailEnd != string::npos && emailEnd > emailStart) {
        attendeeEmail = attendeeInfo.substr(emailStart + 1, emailEnd - emailStart - 1);
    } else {
        attendeeEmail = attendeeInfo;
    }

    // The parser may preserve the iCalendar URI scheme. Compare actual mailbox
    // addresses rather than treating "MAILTO:user@example.com" as a different
    // sender from "user@example.com".
    string lowerAttendeePrefix = attendeeEmail;
    transform(lowerAttendeePrefix.begin(), lowerAttendeePrefix.end(), lowerAttendeePrefix.begin(), ::tolower);
    if (lowerAttendeePrefix.find("mailto:") == 0) {
        attendeeEmail = attendeeEmail.substr(7);
    }

    // Validation 7: Verify From address matches ATTENDEE email (RFC 6047 requirement)
    // Mismatches may cause the RSVP to be rejected by the organizer's calendar
    string fromEmail = account->emailAddress();
    string lowerFromEmail = fromEmail;
    string lowerAttendeeEmail = attendeeEmail;
    transform(lowerFromEmail.begin(), lowerFromEmail.end(), lowerFromEmail.begin(), ::tolower);
    transform(lowerAttendeeEmail.begin(), lowerAttendeeEmail.end(), lowerAttendeeEmail.begin(), ::tolower);

    if (lowerFromEmail != lowerAttendeeEmail) {
        // Warn but don't fail - email aliases and forwarding may cause legitimate mismatches
        logger->warn("RSVP From address ({}) does not match ATTENDEE email in ICS ({}). "
                     "This may cause the RSVP to be rejected.", fromEmail, attendeeEmail);
    }

    // =========================================================================
    // Build RFC 6047-compliant iMIP message with multipart/alternative structure
    // =========================================================================

    // Extract event summary for human-readable message
    string eventSummary = event->Summary.empty() ? "Calendar Event" : event->Summary;

    // Generate human-readable text based on RSVP status (per RFC 6047 recommendation)
    string humanReadableText;
    if (icsRSVPStatus == "ACCEPTED") {
        humanReadableText = fromEmail + " has accepted the invitation to: " + eventSummary;
    } else if (icsRSVPStatus == "DECLINED") {
        humanReadableText = fromEmail + " has declined the invitation to: " + eventSummary;
    } else if (icsRSVPStatus == "TENTATIVE") {
        humanReadableText = fromEmail + " has tentatively accepted the invitation to: " + eventSummary;
    } else {
        humanReadableText = fromEmail + " has responded to the invitation: " + eventSummary;
    }

    // Generate a unique boundary for multipart message
    string boundary = "----=_SummerMail_RSVP_" + to_string(time(0)) + "_" + to_string(rand());

    // Base64 encode the ICS data (RFC 6047 recommends base64 for maximum compatibility)
    Data * icsData = AS_MCSTR(ics)->dataUsingEncoding("utf-8");
    String * icsBase64 = icsData->base64String();

    // Build MIME headers
    MessageBuilder builder;
    builder.header()->setSubject(AS_MCSTR(subject));
    builder.header()->setUserAgent(MCSTR("SummerMail"));
    builder.header()->setDate(time(0));

    Array * toArray = Array::array();
    toArray->addObject(Address::addressWithMailbox(AS_MCSTR(organizer)));
    builder.header()->setTo(toArray);

    Address * me = Address::addressWithMailbox(AS_MCSTR(fromEmail));
    builder.header()->setFrom(me);
    builder.header()->setReplyTo(Array::arrayWithObject(me));

    // Construct the multipart/alternative body per RFC 6047 Section 2.4
    // Structure: text/plain (human-readable) + text/calendar (machine-readable)
    stringstream mimeBody;
    mimeBody << "--" << boundary << "\r\n";
    mimeBody << "Content-Type: text/plain; charset=UTF-8\r\n";
    mimeBody << "Content-Transfer-Encoding: 7bit\r\n";
    mimeBody << "\r\n";
    mimeBody << humanReadableText << "\r\n";
    mimeBody << "\r\n";
    mimeBody << "--" << boundary << "\r\n";
    // Critical: Content-Type MUST include method=REPLY parameter (RFC 6047 Section 2.4)
    mimeBody << "Content-Type: text/calendar; method=REPLY; charset=UTF-8\r\n";
    mimeBody << "Content-Transfer-Encoding: base64\r\n";
    // Use inline disposition, not attachment (RFC 6047 Section 2.4)
    mimeBody << "Content-Disposition: inline; filename=\"invite.ics\"\r\n";
    mimeBody << "\r\n";
    mimeBody << icsBase64->UTF8Characters() << "\r\n";
    mimeBody << "--" << boundary << "--\r\n";

    // Build the complete message by getting headers and appending our body
    // We set a dummy body first, then replace it
    builder.setTextBody(MCSTR("placeholder"));
    Data * headerData = builder.data();
    string headerStr = string(headerData->bytes(), headerData->length());

    // Find where headers end (double CRLF) and extract just the headers
    size_t headerEnd = headerStr.find("\r\n\r\n");
    if (headerEnd == string::npos) {
        headerEnd = headerStr.find("\n\n");
    }

    // Reconstruct message with correct Content-Type and our multipart body
    stringstream fullMessage;

    // Write original headers but replace Content-Type
    string headers = headerStr.substr(0, headerEnd);

    // Remove the original Content-Type and Content-Transfer-Encoding headers
    string newHeaders;
    istringstream headerStream(headers);
    string line;
    while (getline(headerStream, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Skip Content-Type and Content-Transfer-Encoding headers (we'll add our own)
        string lowerLine = line;
        transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);
        if (lowerLine.find("content-type:") == 0 || lowerLine.find("content-transfer-encoding:") == 0) {
            continue;
        }
        newHeaders += line + "\r\n";
    }

    fullMessage << newHeaders;
    fullMessage << "Content-Type: multipart/alternative; boundary=\"" << boundary << "\"\r\n";
    fullMessage << "MIME-Version: 1.0\r\n";
    fullMessage << "\r\n";
    fullMessage << mimeBody.str();

    string messageStr = fullMessage.str();
    Data * messageData = Data::dataWithBytes(messageStr.c_str(), (unsigned int)messageStr.length());

    // =========================================================================
    // Send the RSVP via SMTP
    // =========================================================================

    SMTPSession smtp;
    SMTPProgress sprogress;
    MailUtils::configureSessionForAccount(smtp, account);

    logger->info("-- Sending RFC 6047-compliant RSVP ({}) to organizer {}", icsRSVPStatus, organizer);
    smtp.sendMessage(messageData, &sprogress, &err);

    if (err != ErrorNone) {
        int e = smtp.lastLibetpanError();
        string es = LibEtPanCodeToTypeMap.count(e) ? LibEtPanCodeToTypeMap[e] : to_string(e);
        logger->info("-X An SMTP error occurred: {} LibEtPan code: {}", ErrorCodeToTypeMap[err], es);
        throw SyncException("send-failed", ErrorCodeToTypeMap[err], false);
    }

    logger->info("-- RSVP sent successfully");
}
