// Linux/macOS half of the updater: curl transfers and file replacement.
// All validation and version comparison live in updater_common.cpp so both
// platforms share one implementation.
#ifndef _WIN32

#include "ewr/updater.h"

#include <curl/curl.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace ewr {
    namespace platform {

        namespace {

            const char* kUserAgent = "EWR-Updater/1.0";

            size_t WriteToStream(void* buffer, size_t size, size_t count, void* userData)
            {
                std::ofstream* stream = static_cast<std::ofstream*>(userData);
                stream->write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size * count));
                return stream->good() ? size * count : 0;
            }

            size_t WriteToString(void* buffer, size_t size, size_t count, void* userData)
            {
                std::string* target = static_cast<std::string*>(userData);
                target->append(static_cast<const char*>(buffer), size * count);
                return size * count;
            }

            void ApplyCommonOptions(CURL* handle, const std::string& url, long timeoutSeconds)
            {
                curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
                curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
                curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
                curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 3L);
                curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeoutSeconds);
                curl_easy_setopt(handle, CURLOPT_USERAGENT, kUserAgent);
                // Required when curl is used from a worker thread.
                curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
            }

            bool CopyThenRemove(const fs::path& from, const fs::path& to)
            {
                std::error_code ec;
                fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
                if (ec) return false;

                fs::remove(from, ec);
                return true;
            }

        } // namespace

        void InitNetworking()
        {
            // curl_global_init is not thread-safe; do it once before any worker runs.
            static std::once_flag initFlag;
            std::call_once(initFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
        }

        bool HttpDownloadToFile(const std::string& url, const std::string& destPath)
        {
            InitNetworking();

            CURL* handle = curl_easy_init();
            if (handle == nullptr) return false;

            std::ofstream output(destPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                curl_easy_cleanup(handle);
                return false;
            }

            ApplyCommonOptions(handle, url, 30L);
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteToStream);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, &output);

            const CURLcode result = curl_easy_perform(handle);
            curl_easy_cleanup(handle);
            output.close();

            if (result != CURLE_OK)
            {
                std::error_code ec;
                fs::remove(destPath, ec);
                return false;
            }

            return true;
        }

        bool HttpGetString(const std::string& url, std::string& outBody)
        {
            InitNetworking();
            outBody.clear();

            CURL* handle = curl_easy_init();
            if (handle == nullptr) return false;

            ApplyCommonOptions(handle, url, 10L);
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteToString);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, &outBody);

            const CURLcode result = curl_easy_perform(handle);
            curl_easy_cleanup(handle);

            return result == CURLE_OK && !outBody.empty();
        }

        bool ReplaceFileAtomic(const std::string& from, const std::string& to)
        {
            std::error_code ec;
            fs::rename(from, to, ec);
            if (!ec) return true;

            // Rename cannot cross filesystems (EXDEV), e.g. /tmp -> /usr/local/bin.
            return CopyThenRemove(fs::path(from), fs::path(to));
        }

    } // namespace platform
} // namespace ewr

#endif // !_WIN32
