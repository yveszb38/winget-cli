// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "SQLiteIndex.h"
#include "Schema/MetadataTable.h"
#include <winget/ManifestYamlParser.h>

namespace AppInstaller::Repository::Microsoft
{
    namespace
    {
        char const* const GetOpenDispositionString(SQLiteIndex::OpenDisposition disposition)
        {
            switch (disposition)
            {
            case AppInstaller::Repository::Microsoft::SQLiteIndex::OpenDisposition::Read:
                return "Read";
            case AppInstaller::Repository::Microsoft::SQLiteIndex::OpenDisposition::ReadWrite:
                return "ReadWrite";
            case AppInstaller::Repository::Microsoft::SQLiteIndex::OpenDisposition::Immutable:
                return "ImmutableRead";
            default:
                return "Unknown";
            }
        }
    }

    SQLiteIndex SQLiteIndex::CreateNew(const std::string& filePath, Schema::Version version)
    {
        AICLI_LOG(Repo, Info, << "Creating new SQLite Index [" << version << "] at '" << filePath << "'");
        SQLiteIndex result{ filePath, version };

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(result.m_dbconn, "sqliteindex_createnew");

        Schema::MetadataTable::Create(result.m_dbconn);
        // Use calculated version, as incoming version could be 'latest'
        result.m_version.SetSchemaVersion(result.m_dbconn);

        result.m_interface->CreateTables(result.m_dbconn);

        result.SetLastWriteTime();

        savepoint.Commit();

        return result;
    }

    SQLiteIndex SQLiteIndex::Open(const std::string& filePath, OpenDisposition disposition)
    {
        AICLI_LOG(Repo, Info, << "Opening SQLite Index for " << GetOpenDispositionString(disposition) << " at '" << filePath << "'");
        switch (disposition)
        {
        case AppInstaller::Repository::Microsoft::SQLiteIndex::OpenDisposition::Read:
            return { filePath, SQLite::Connection::OpenDisposition::ReadOnly, SQLite::Connection::OpenFlags::None };
        case AppInstaller::Repository::Microsoft::SQLiteIndex::OpenDisposition::ReadWrite:
            return { filePath, SQLite::Connection::OpenDisposition::ReadWrite, SQLite::Connection::OpenFlags::None };
        case AppInstaller::Repository::Microsoft::SQLiteIndex::OpenDisposition::Immutable:
        {
            // Following the algorithm set forth at https://sqlite.org/uri.html [3.1] to convert to a URI path
            // The execution order builds out the string so that it shouldn't require any moves (other than growing)
            std::string target;
            // Add an 'arbitrary' growth size to prevent the majority of needing to grow (adding 'file:/' and '?immutable=1')
            target.reserve(filePath.size() + 20);

            target += "file:";

            bool wasLastCharSlash = false;

            if (filePath.size() >= 2 && filePath[1] == ':' &&
                ((filePath[0] >= 'a' && filePath[0] <= 'z') ||
                 (filePath[0] >= 'A' && filePath[0] <= 'Z')))
            {
                target += '/';
                wasLastCharSlash = true;
            }

            for (char c : filePath)
            {
                bool wasThisCharSlash = false;
                switch (c)
                {
                case '?': target += "%3f"; break;
                case '#': target += "%23"; break;
                case '\\':
                case '/':
                {
                    wasThisCharSlash = true;
                    if (!wasLastCharSlash)
                    {
                        target += '/';
                    }
                    break;
                }
                default: target += c; break;
                }

                wasLastCharSlash = wasThisCharSlash;
            }

            target += "?immutable=1";

            return { target, SQLite::Connection::OpenDisposition::ReadOnly, SQLite::Connection::OpenFlags::Uri };
        }
        default:
            THROW_HR(E_UNEXPECTED);
        }
    }

    SQLiteIndex::SQLiteIndex(const std::string& target, SQLite::Connection::OpenDisposition disposition, SQLite::Connection::OpenFlags flags) :
        m_dbconn(SQLite::Connection::Create(target, disposition, flags))
    {
        m_dbconn.EnableICU();
        m_version = Schema::Version::GetSchemaVersion(m_dbconn);
        AICLI_LOG(Repo, Info, << "Opened SQLite Index with version [" << m_version << "], last write [" << GetLastWriteTime() << "]");
        m_interface = m_version.CreateISQLiteIndex();
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_CANNOT_WRITE_TO_UPLEVEL_INDEX, disposition == SQLite::Connection::OpenDisposition::ReadWrite && m_version != m_interface->GetVersion());
    }

    SQLiteIndex::SQLiteIndex(const std::string& target, Schema::Version version) :
        m_dbconn(SQLite::Connection::Create(target, SQLite::Connection::OpenDisposition::Create))
    {
        m_dbconn.EnableICU();
        m_interface = version.CreateISQLiteIndex();
        m_version = m_interface->GetVersion();
    }

    void SQLiteIndex::AddManifest(const std::filesystem::path& manifestPath, const std::filesystem::path& relativePath)
    {
        AICLI_LOG(Repo, Info, << "Adding manifest from file [" << manifestPath << "]");

        Manifest::Manifest manifest = Manifest::YamlParser::CreateFromPath(manifestPath);
        AddManifest(manifest, relativePath);
    }

    void SQLiteIndex::AddManifest(const Manifest::Manifest& manifest, const std::filesystem::path& relativePath)
    {
        AICLI_LOG(Repo, Info, << "Adding manifest for [" << manifest.Id << ", " << manifest.Version << "] at relative path [" << relativePath << "]");

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(m_dbconn, "sqliteindex_addmanifest");

        m_interface->AddManifest(m_dbconn, manifest, relativePath);

        SetLastWriteTime();

        savepoint.Commit();
    }

    bool SQLiteIndex::UpdateManifest(const std::filesystem::path& manifestPath, const std::filesystem::path& relativePath)
    {
        AICLI_LOG(Repo, Info, << "Updating manifest from file [" << manifestPath << "]");

        Manifest::Manifest manifest = Manifest::YamlParser::CreateFromPath(manifestPath);
        return UpdateManifest(manifest, relativePath);
    }

    bool SQLiteIndex::UpdateManifest(const Manifest::Manifest& manifest, const std::filesystem::path& relativePath)
    {
        AICLI_LOG(Repo, Info, << "Updating manifest for [" << manifest.Id << ", " << manifest.Version << "] at relative path [" << relativePath << "]");

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(m_dbconn, "sqliteindex_updatemanifest");

        bool result = m_interface->UpdateManifest(m_dbconn, manifest, relativePath);

        if (result)
        {
            SetLastWriteTime();

            savepoint.Commit();
        }

        return result;
    }

    void SQLiteIndex::RemoveManifest(const std::filesystem::path& manifestPath, const std::filesystem::path& relativePath)
    {
        AICLI_LOG(Repo, Info, << "Removing manifest from file [" << manifestPath << "]");

        Manifest::Manifest manifest = Manifest::YamlParser::CreateFromPath(manifestPath);
        RemoveManifest(manifest, relativePath);
    }

    void SQLiteIndex::RemoveManifest(const Manifest::Manifest& manifest, const std::filesystem::path& relativePath)
    {
        AICLI_LOG(Repo, Info, << "Removing manifest for [" << manifest.Id << ", " << manifest.Version << "] at relative path [" << relativePath << "]");

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(m_dbconn, "sqliteindex_removemanifest");

        m_interface->RemoveManifest(m_dbconn, manifest, relativePath);

        SetLastWriteTime();

        savepoint.Commit();
    }

    void SQLiteIndex::PrepareForPackaging()
    {
        AICLI_LOG(Repo, Info, << "Preparing index for packaging");

        m_interface->PrepareForPackaging(m_dbconn);
    }

    Schema::ISQLiteIndex::SearchResult SQLiteIndex::Search(const SearchRequest& request)
    {
        AICLI_LOG(Repo, Info, << "Performing search: " << request.ToString());

        return m_interface->Search(m_dbconn, request);
    }

    std::optional<std::string> SQLiteIndex::GetIdStringById(IdType id)
    {
        return m_interface->GetIdStringById(m_dbconn, id);
    }

    std::optional<std::string> SQLiteIndex::GetNameStringById(IdType id)
    {
        return m_interface->GetNameStringById(m_dbconn, id);
    }

    std::optional<std::string> SQLiteIndex::GetPathStringByKey(IdType id, std::string_view version, std::string_view channel)
    {
        return m_interface->GetPathStringByKey(m_dbconn, id, version, channel);
    }

    std::vector<Utility::VersionAndChannel> SQLiteIndex::GetVersionsById(IdType id)
    {
        return m_interface->GetVersionsById(m_dbconn, id);
    }

    // Recording last write time based on MSDN documentation stating that time returns a POSIX epoch time and thus
    // should be consistent across systems.
    void SQLiteIndex::SetLastWriteTime()
    {
        Schema::MetadataTable::SetNamedValue(m_dbconn, Schema::s_MetadataValueName_LastWriteTime, Utility::GetCurrentUnixEpoch());
    }

    std::chrono::system_clock::time_point SQLiteIndex::GetLastWriteTime()
    {
        int64_t lastWriteTime = Schema::MetadataTable::GetNamedValue<int64_t>(m_dbconn, Schema::s_MetadataValueName_LastWriteTime);
        return Utility::ConvertUnixEpochToSystemClock(lastWriteTime);
    }
}
