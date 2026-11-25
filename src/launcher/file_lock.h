#ifndef LAUNCHER_FILE_LOCK_H_
#define LAUNCHER_FILE_LOCK_H_

// Held while a mod's files are fetched and written: the Vita stays awake and the PS button cannot leave the app.

namespace ug2 { namespace launcher {

class FileLock
{
public:
    virtual ~FileLock() {}
    virtual void take() = 0;
    virtual void release() = 0;
};

// The Vita's lock; on the host it does nothing.
FileLock& systemLock();

}}

#endif
