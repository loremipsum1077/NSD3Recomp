// nn::friends: the console's friend list and presence. The port has no Nintendo
// network account, so the list is empty and everything asynchronous fails at once.
//
// The game owns these objects and decides their size; nothing here stores anything
// in them beyond what the notes say, and every getter answers without reading them.
#include "nn/os/os_types.h"

#include <string.h>

namespace nn { namespace os {
void InitializeMutex(MutexType * mutex, bool recursive, int lockLevel);
void InitializeEvent(EventType * event, bool signaled, EventClearMode mode);
}}

namespace {
// Module 121 (friends), "not available".
nn::Result Unavailable() { return nn::Result{121u | 1u << 9}; }
const nn::account::Nickname kNoName = {};
const unsigned long kNoPresence[0xe0 / 8] = {};
}

namespace nn { namespace friends {

enum PresenceStatus { PresenceStatus_Offline = 0 };
struct PlayHistoryRegistrationKey;
struct InAppScreenName;
struct FriendFilter;
struct NotificationInfo;

class FriendPresence {              // game size 0xe0
public:
    PresenceStatus GetStatus() const;
    bool IsSamePresenceGroupApplication() const;
};

PresenceStatus FriendPresence::GetStatus() const { return PresenceStatus_Offline; }
bool FriendPresence::IsSamePresenceGroupApplication() const { return false; }

class Friend {                      // game size 0x200
public:
    Friend();
    account::NetworkServiceAccountId GetAccountId() const;
    const account::Nickname & GetNickname() const;
    const FriendPresence & GetPresence() const;
    bool IsFavorite() const;
    bool IsValid() const;
};

Friend::Friend() {}
account::NetworkServiceAccountId Friend::GetAccountId() const { return account::NetworkServiceAccountId{0}; }
const account::Nickname & Friend::GetNickname() const { return kNoName; }
const FriendPresence & Friend::GetPresence() const { return *reinterpret_cast<const FriendPresence *>(kNoPresence); }
bool Friend::IsFavorite() const { return false; }
bool Friend::IsValid() const { return false; }

class Profile {
public:
    Profile();
    account::NetworkServiceAccountId GetAccountId() const;
    const account::Nickname & GetNickname() const;
    bool IsValid() const;
};

Profile::Profile() {}
account::NetworkServiceAccountId Profile::GetAccountId() const { return account::NetworkServiceAccountId{0}; }
const account::Nickname & Profile::GetNickname() const { return kNoName; }
bool Profile::IsValid() const { return false; }

class AsyncContext {                // game size 0x28: an os mutex, then a pointer
public:
    AsyncContext();
    ~AsyncContext();
    Result Cancel();
    Result HasDone(bool * done);
    Result GetResult() const;

private:
    os::MutexType m_mutex;
    void * m_internal;
};
static_assert(sizeof(AsyncContext) == 0x28, "AsyncContext");

AsyncContext::AsyncContext() : m_internal(nullptr) { os::InitializeMutex(&m_mutex, false, 0); }
AsyncContext::~AsyncContext() {}
Result AsyncContext::Cancel() { return ResultSuccess(); }

Result AsyncContext::HasDone(bool * done)
{
    *done = true;
    return ResultSuccess();
}

Result AsyncContext::GetResult() const { return Unavailable(); }

class NotificationQueue {           // game size 0x68
public:
    NotificationQueue();
    Result Initialize(const account::Uid & user);
    void Finalize();
    Result Pop(NotificationInfo * out);

private:
    os::MutexType m_mutex;          // 0x00
    account::Uid m_user;            // 0x20
    void * m_handle;                // 0x30
    os::SystemEventType m_event;    // 0x38: the game can wait on this; it never fires
};
static_assert(sizeof(NotificationQueue) == 0x68, "NotificationQueue");

NotificationQueue::NotificationQueue() : m_user{}, m_handle(nullptr)
{
    os::InitializeMutex(&m_mutex, false, 0);
    memset(&m_event, 0, sizeof(m_event));
    os::InitializeEvent(&m_event.event, false, os::EventClearMode_ManualClear);
}

Result NotificationQueue::Initialize(const account::Uid & user)
{
    m_user = user;
    return ResultSuccess();
}

void NotificationQueue::Finalize() {}

// Always empty.
Result NotificationQueue::Pop(NotificationInfo *) { return Unavailable(); }

Result GetFriendList(int * count, Friend *, const account::Uid &, int, int, const FriendFilter &)
{
    *count = 0;
    return ResultSuccess();
}

Result UpdateFriendInfo(Friend *, const account::Uid &, const account::NetworkServiceAccountId *, int) { return Unavailable(); }
Result GetProfileList(AsyncContext *, Profile *, const account::Uid &, const account::NetworkServiceAccountId *, int) { return Unavailable(); }
Result GetPlayHistoryRegistrationKey(PlayHistoryRegistrationKey *, const account::Uid &, bool) { return Unavailable(); }
Result AddPlayHistory(const account::Uid &, const PlayHistoryRegistrationKey &, const InAppScreenName &, const InAppScreenName &) { return ResultSuccess(); }
Result ShowUserDetailInfo(const account::Uid &, const account::NetworkServiceAccountId &, const InAppScreenName &, const InAppScreenName &) { return Unavailable(); }

}}  // namespace nn::friends
