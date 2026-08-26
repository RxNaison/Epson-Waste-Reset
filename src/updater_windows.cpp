// Windows half of the updater: WinINet transfers and file replacement.
// All validation and version comparison live in updater_common.cpp so both
// platforms share one implementation.
#ifdef _WIN32

#include "ewr/updater.h"

#include <windows.h>
#include <wininet.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ewr {
    namespace platform {

        namespace {

            const char* kUserAgent = "EWR-Updater/1.0";
            constexpr DWORD kTimeoutMs = 10000;
            constexpr DWORD kConnectTimeoutMs = 3000;

            struct InternetHandle
            {
                HINTERNET handle = nullptr;

                explicit InternetHandle(HINTERNET h) : handle(h) {}
                ~InternetHandle() { if (handle != nullptr) InternetCloseHandle(handle); }

                InternetHandle(const InternetHandle&) = delete;
                InternetHandle& operator=(const InternetHandle&) = delete;

                explicit operator bool() const { return handle != nullptr; }
            };

            bool OpenUrl(const std::string& url, InternetHandle& session, InternetHandle& request)
            {
                session.handle = InternetOpenA(kUserAgent, INTERNET_OPEN_TYPE_PRECONFIG,
                                               nullptr, nullptr, 0);
                if (!session) return false;

                DWORD connectTimeout = kConnectTimeoutMs;
                DWORD receiveTimeout = kTimeoutMs;
                InternetSetOptionA(session.handle, INTERNET_OPTION_CONNECT_TIMEOUT,
                                   &connectTimeout, sizeof(connectTimeout));
                InternetSetOptionA(session.handle, INTERNET_OPTION_RECEIVE_TIMEOUT,
                                   &receiveTimeout, sizeof(receiveTimeout));

                request.handle = InternetOpenUrlA(session.handle, url.c_str(), nullptr, 0,
                                                  INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
                if (!request) return false;

                // Treat 4xx/5xx as failure instead of saving the error page.
                DWORD statusCode = 0;
                DWORD statusSize = sizeof(statusCode);
                if (HttpQueryInfoA(request.handle, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                                   &statusCode, &statusSize, nullptr) &&
                    statusCode >= 400)
                    return false;

                return true;
            }

        } // namespace

        void InitNetworking()
        {
            // WinINet needs no global initialization.
        }

        bool HttpDownloadToFile(const std::string& url, const std::string& destPath)
        {
            InternetHandle session(nullptr);
            InternetHandle request(nullptr);
            if (!OpenUrl(url, session, request))
                return false;

            std::ofstream output(destPath, std::ios::binary | std::ios::trunc);
            if (!output) return false;

            std::vector<char> buffer(32768);
            DWORD bytesRead = 0;

            while (InternetReadFile(request.handle, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), &bytesRead) &&
                   bytesRead > 0)
            {
                output.write(buffer.data(), static_cast<std::streamsize>(bytesRead));
                if (!output)
                {
                    output.close();
                    std::error_code ec;
                    fs::remove(destPath, ec);
                    return false;
                }
            }

            output.close();

            std::error_code ec;
            if (fs::file_size(destPath, ec) == 0 || ec)
            {
                fs::remove(destPath, ec);
                return false;
            }

            return true;
        }

        bool HttpGetString(const std::string& url, std::string& outBody)
        {
            outBody.clear();

            InternetHandle session(nullptr);
            InternetHandle request(nullptr);
            if (!OpenUrl(url, session, request))
                return false;

            std::vector<char> buffer(8192);
            DWORD bytesRead = 0;

            while (InternetReadFile(request.handle, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), &bytesRead) &&
                   bytesRead > 0)
                outBody.append(buffer.data(), bytesRead);

            return !outBody.empty();
        }

        bool ReplaceFileAtomic(const std::string& from, const std::string& to)
        {
            // COPY_ALLOWED is required when source and target are on different
            // volumes; without it MoveFileEx fails with ERROR_NOT_SAME_DEVICE.
            return MoveFileExA(from.c_str(), to.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE;
        }

    } // namespace platform
} // namespace ewr

#endif // _WIN32
