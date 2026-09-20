#include "ewr/run_lock.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ewr {

#ifdef _WIN32

    // "Local\" scopes the name to the logon session, which is as far as one
    // user's printer reaches. A mutex the owner never releases is exactly the
    // point: it dies with the process, including one that was killed.
    RunLock::RunLock()
    {
        HANDLE mutex = CreateMutexA(nullptr, TRUE, "Local\\EpsonWasteReset.PrinterRun");
        if (!mutex)
        {
            m_held = true; // cannot lock at all - do not block the run over it
            return;
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(mutex);
            return;
        }

        m_handle = mutex;
        m_held = true;
    }

    RunLock::~RunLock()
    {
        if (!m_handle)
            return;

        ReleaseMutex(static_cast<HANDLE>(m_handle));
        CloseHandle(static_cast<HANDLE>(m_handle));
    }

#else

    namespace {

        std::string LockPath()
        {
            const char* dir = std::getenv("TMPDIR");
            if (!dir || !*dir)
                dir = "/tmp";

            return std::string(dir) + "/epson-waste-reset.lock";
        }

    } // namespace

    // flock, not a lock file's existence: the kernel drops it when the process
    // dies, so a killed run leaves nothing to clean up. The file itself stays
    // behind and is meant to.
    RunLock::RunLock()
    {
        const int fd = ::open(LockPath().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (fd < 0)
        {
            m_held = true; // read-only /tmp, a sandbox - not a reason to stop
            return;
        }

        if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(fd);
            return;
        }

        m_fd = fd;
        m_held = true;
    }

    RunLock::~RunLock()
    {
        if (m_fd < 0)
            return;

        ::flock(m_fd, LOCK_UN);
        ::close(m_fd);
    }

#endif

} // namespace ewr
