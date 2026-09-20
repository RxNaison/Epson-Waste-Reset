#pragma once
#include <string>

namespace ewr {

    // One EWR run at a time, machine-wide.
    //
    // usbprint.sys is opened shared - the spooler and Epson's status monitor
    // hold the printer open and an exclusive open just fails - so two runs can
    // drive one printer at once, and whichever has a read pending takes the
    // other's reply. A lost reply leaves the D4 credit books wrong on both
    // sides, which is issue #39; the run that loses one may be the one halfway
    // through a write. A stale run counts: a leftover EWR waiting at a prompt
    // is invisible to anyone looking for Epson software by name.
    //
    // Held() is the whole contract: false means another run has it, not that
    // anything went wrong. When the lock cannot be created at all (no
    // temp directory, a sandbox), it reports itself held rather than stopping
    // a reset over a lock file.
    class RunLock
    {
    public:
        RunLock();
        ~RunLock();

        RunLock(const RunLock&) = delete;
        RunLock& operator=(const RunLock&) = delete;

        bool Held() const { return m_held; }

    private:
        bool m_held = false;
        // Windows: HANDLE. POSIX: file descriptor. -1 / null when unheld.
        void* m_handle = nullptr;
        int m_fd = -1;
    };

} // namespace ewr
