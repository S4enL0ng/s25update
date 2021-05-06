// Copyright (C) 2005 - 2021 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "s25update.h" // IWYU pragma: keep
#include "md5sum.h"
#include "s25util/warningSuppression.h"
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/optional.hpp>
#include <algorithm>
#include <array>
#include <bzlib.h>
#include <curl/curl.h>
#include <iomanip>
#include <sstream>
#include <vector>
#ifdef _WIN32
#    include <windows.h>
#    include <shellapi.h>
#endif

namespace bfs = boost::filesystem;
namespace bnw = boost::nowide;

#ifndef TARGET
#    ifdef _WIN32
#        define TARGET "windows"
#    endif

#    ifdef __APPLE__
#        define TARGET "apple"
#    endif

#    ifdef __linux__
#        define TARGET "linux"
#    endif
#endif

#ifndef TARGET
#    error You have to set TARGET to your platform (windows/linux/apple)
#endif

#ifndef ARCH
#    error You have to set ARCH to your architecture (i386/x86_64/ppc)
#endif

#define HTTPHOST "https://nightly.siedler25.org/s25client/"
#define STABLEPATH "stable/"
#define NIGHTLYPATH "nightly/"
#define FILEPATH "/updater"
#define FILELIST "/files"
#define LINKLIST "/links"
#define SAVEGAMEVERSION "/savegameversion"

#ifndef SEE_MASK_NOASYNC
#    define SEE_MASK_NOASYNC 0x00000100
#endif

#ifdef _WIN32
/**
 *  \r fix-function for the stupid windows-console
 *  NOT THREADSAFE!!!
 */
static short backslashfix_y;

static short backslashrfix(short y)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hConsoleOutput;
    hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(hConsoleOutput, &csbi);

    COORD Cursor_an_Pos;
    Cursor_an_Pos.X = 0;
    Cursor_an_Pos.Y = csbi.dwCursorPosition.Y + y;
    SetConsoleCursorPosition(hConsoleOutput, Cursor_an_Pos);

    return csbi.dwCursorPosition.Y + y;
}

#endif // !_WIN32

/**
 *  curl filewriter callback
 */
static size_t WriteCallback(void* ptr, size_t size, size_t nmemb, FILE* stream)
{
    size_t realsize = size * nmemb;

    if(stream && realsize == fwrite(ptr, size, nmemb, stream))
        return realsize;

    return 0;
}

/**
 *  curl std::stringwriter callback
 */
static size_t WriteMemoryCallback(void* ptr, size_t size, size_t nmemb, std::string* data)
{
    size_t realsize = size * nmemb;

    std::string tmp(reinterpret_cast<char*>(ptr), realsize);
    *data += tmp;

    return realsize;
}

/**
 *  curl progressbar callback
 */
static int ProgressBarCallback(std::string* data, double dltotal, double dlnow, double /*ultotal*/, double /*ulnow*/)
{
#ifdef _WIN32
    // \r not working fix
    if(backslashrfix(0) != backslashfix_y)
        backslashfix_y = backslashrfix(-1);
#endif // !_WIN32

    bnw::cout << "\r" << *data;
    if(dltotal > 0) /* Avoid division by zero */
        bnw::cout << std::setw(5) << std::setprecision(2) << std::setiosflags(std::ios::fixed)
                  << (dlnow * 100.0 / dltotal) << "%";
    bnw::cout << std::flush;

    return 0;
}

/**
 *  curl escape wrapper
 */
static std::string EscapeFile(const std::string& file)
{
    CURL* curl_handle = curl_easy_init();
    char* escaped = curl_easy_escape(curl_handle, file.c_str(), static_cast<int>(file.length()));
    std::string result;
    if(escaped)
    {
        result = escaped;
        curl_free(escaped);
    }

    curl_easy_cleanup(curl_handle);

    return result;
}

/**
 *  httpdownload function (to std::string or to file, with or without progressbar)
 */
static bool DoDownloadFile(const std::string& url, std::string* to, const bfs::path& path = "",
                           std::string* progress = nullptr)
{
    FILE* tofp = nullptr;
    bool ok = true;

    bfs::path tmpPath = path;
    tmpPath += ".new";

    CURL* curl_handle = curl_easy_init();

    curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str()); //-V111
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "s25update/1.1");
    curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1);

    // Write file to Memory?
    if(path.empty())
    {
        if(!to)
            throw std::logic_error("No target for download given");
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback); //-V111
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, static_cast<void*>(to));  //-V111
    } else
    {
        tofp = boost::nowide::fopen(tmpPath.string().c_str(), "wb");
        if(!tofp)
        {
            bnw::cerr << "Can't open file \"" << tmpPath << "\"!!!!" << std::endl;
            ok = false;
        }
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteCallback);        //-V111
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, static_cast<void*>(tofp)); //-V111
    }

    // Show Progress?
    if(progress)
    {
        curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl_handle, CURLOPT_PROGRESSFUNCTION, ProgressBarCallback);      //-V111
        curl_easy_setopt(curl_handle, CURLOPT_PROGRESSDATA, static_cast<void*>(progress)); //-V111
    }

    // curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 1L);

    if(ok)
        ok = curl_easy_perform(curl_handle) == 0;

    curl_easy_cleanup(curl_handle);

    if(!path.empty())
    {
        if(tofp)
            fclose(tofp);
        if(ok)
            rename(tmpPath, path);
    }

    return ok;
}

static bool DownloadFile(const std::string& url, const bfs::path& path, std::string progress = "")
{
    return DoDownloadFile(url, nullptr, path, &progress);
}

static boost::optional<std::string> DownloadFile(const std::string& url)
{
    std::string tmp;
    if(DoDownloadFile(url, &tmp))
        return tmp;
    else
        return boost::none;
}

/**
 *  calculate md5sum for a file
 */
std::string md5sum(const std::string& file)
{
    std::string digest;

    FILE* fp = boost::nowide::fopen(file.c_str(), "rb");
    if(fp)
    {
        md5file(fp, digest);
        fclose(fp);
    }
    return digest;
}

#ifdef _WIN32
/**
 *  get the last error (win only)
 */
std::string get_last_error_string()
{
    LPVOID lpMsgBuf;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                   GetLastError(),
                   0, // Default language
                   (LPSTR)&lpMsgBuf, 0, NULL);
    std::string result = (LPCSTR)lpMsgBuf;
    // Free the buffer.
    LocalFree(lpMsgBuf);
    return result;
}
#endif

// Checks the savegame version and return true if update can continue
bool ValidateSavegameVersion(const std::string& httpbase, const bfs::path& savegameversionFilePath)
{
    // check new savegame version before downloading
    const auto remote_savegameversion_content = DownloadFile(httpbase + SAVEGAMEVERSION);
    if(!remote_savegameversion_content)
    {
        bnw::cerr << "Error: Was not able to get remote savegame version, ignoring for now" << std::endl;
        return true;
        // return false; // uncomment this if it actually works (to not break updater now)
    }
    bnw::ifstream local_savegame_version(savegameversionFilePath);
    std::stringstream remote_savegame_version(*remote_savegameversion_content);
    int localVersion, remoteVersion;
    if(!(local_savegame_version >> localVersion && remote_savegame_version >> remoteVersion))
    {
        local_savegame_version.seekg(0);
        std::string curVersion;
        local_savegame_version >> curVersion;
        bnw::cerr << "Error: Could not parse savegame versions" << std::endl
                  << "Current: " << curVersion << std::endl
                  << "Update:  " << *remote_savegameversion_content << std::endl;
    } else
    {
        bnw::cout << "Savegame version of currently installed version: " << localVersion << std::endl;
        bnw::cout << "Savegame version of updated version: " << remoteVersion << std::endl;
        if(localVersion == remoteVersion)
        {
            bnw::cout << "You will be able to load your existing savegames." << std::endl;
            return true;
        }
        bnw::cout << "Warning: You will not be able to load your existing savegames. " << std::endl;
    }
    bnw::cout << "Cancel update? (y/n) ";
    auto input = static_cast<char>(bnw::cin.get());
    if(input != 'n' && input != 'N')
    {
        bnw::cout << std::endl
                  << "Canceling update." << std::endl
                  << "Warning: You will not be able to play with players using a newer version." << std::endl;
        return false;
    } else
    {
        bnw::cout << std::endl << "Continuing update." << std::endl;
        return true;
    }
}

bool isCurrentDirWritable()
{
#ifdef _WIN32
    HANDLE hFile = CreateFileW(L"write.test", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if(hFile == INVALID_HANDLE_VALUE)
    {
        if(GetLastError() != ERROR_ACCESS_DENIED)
            throw std::runtime_error(get_last_error_string());
        return false;
    } else
    {
        CloseHandle(hFile);
        DeleteFileW(L"write.test");
        return true;
    }
#else
    bnw::ofstream testFile("write.test", bnw::ofstream::trunc);
    if(testFile)
    {
        testFile.close();
        bfs::remove("write.test");
        return true;
    } else
        return false;
#endif
}

bool runAsAdmin(int argc, char* argv[])
{
#ifdef _WIN32
    std::stringstream arguments;
    for(int i = 1; i < argc; ++i)
        arguments << argv[i];

    // Launch itself as administrator.
    SHELLEXECUTEINFOA sei = {sizeof(sei)};
    sei.lpVerb = "runas";
    sei.lpFile = argv[0];
    sei.hwnd = GetConsoleWindow();
    sei.lpParameters = arguments.str().c_str();
    sei.nShow = SW_NORMAL;
    sei.fMask = SEE_MASK_NOASYNC;

    if(ShellExecuteExA(&sei))
        return true;
    DWORD dwError = GetLastError();
    if(dwError == ERROR_CANCELLED)
        bnw::cerr << "You refused to elevate - cannot update" << std::endl;
#else
    RTTR_UNUSED(argc);
    RTTR_UNUSED(argv);
#endif
    return false;
}

auto parseFileList(const std::string& filelistFileContents)
{
    std::vector<std::pair<std::string, std::string>> files;
    std::stringstream flstream(filelistFileContents);

    std::string line;
    while(getline(flstream, line))
    {
        if(line.empty())
            break;

        if(line.substr(32, 2) != "  ")
            throw std::runtime_error("Invalid line in filelist: " + line);
        std::string hash = line.substr(0, 32);
        std::string file = line.substr(34);

        files.emplace_back(std::move(hash), std::move(file));

        if(flstream.fail())
            break;
    }
    return files;
}

auto parseLinkList(const std::string& linkFileContents)
{
    // Format: <symlinkFilePath> <linkTarget>
    std::vector<std::pair<std::string, std::string>> links;
    std::stringstream llstream(linkFileContents);
    std::string line;
    while(getline(llstream, line))
    {
        if(line.empty())
            break;
        const auto spacePos = line.find(' ');
        std::string linkTarget = line.substr(spacePos + 1);
        std::string symlinkFilePath = line.substr(0, spacePos);

        links.emplace_back(std::move(symlinkFilePath), std::move(linkTarget));

        if(llstream.fail())
            break;
    }
    return links;
}

void updateFile(const std::string& httpBase, const std::string& origFilePath, const bool verbose)
{
    const bfs::path filepath = bfs::path(origFilePath).make_preferred();
    const bfs::path name = filepath.filename();
    const bfs::path path = filepath.parent_path();
    bfs::path bzfile = filepath;
    bzfile += ".bz2";

    bnw::cout << "Updating " << name;
    if(verbose)
        bnw::cout << " to " << path;
    bnw::cout << std::endl;

    // create path of file
    if(!bfs::is_directory(path))
    {
        boost::system::error_code ec;
        bfs::create_directories(path, ec);
        if(ec)
        {
            std::stringstream msg;
            msg << "Failed to create directories to path " << path << " for " << name << ": " << ec.message()
                << std::endl;
            throw std::runtime_error(msg.str());
        }
    }

    std::stringstream progress;
    progress << "Downloading " << name;
    while(progress.str().size() < 50)
        progress << " ";

    std::stringstream url;
    url << httpBase << "/" << bfs::path(origFilePath).parent_path().string() << "/" << EscapeFile(name.string())
        << ".bz2";

    // download the file
    bool dlOk = DownloadFile(url.str(), bzfile, progress.str());

    bnw::cout << " - ";
    if(!dlOk)
    {
        bnw::cout << "failed!" << std::endl;
        throw std::runtime_error("Download of " + bzfile.string() + "failed!");
    }

    // extract the file
    int bzerror = BZ_OK;
    FILE* bzfp = boost::nowide::fopen(bzfile.string().c_str(), "rb");
    if(!bzfp)
        throw std::runtime_error("decompression failed: download failure?");

    bzerror = BZ_OK;
    BZFILE* bz2fp = BZ2_bzReadOpen(&bzerror, bzfp, 0, 0, nullptr, 0);
    if(!bz2fp)
        throw std::runtime_error("decompression failed: compressed file corrupt?");

    bnw::ofstream outputFile(filepath, bnw::ofstream::binary | bnw::ofstream::trunc);
    if(!outputFile)
    {
        bfs::path bakFilePath(filepath);
        bakFilePath += ".bak";
        boost::system::error_code error;
        bfs::rename(filepath, bakFilePath, error);
        // move file out of the way ...
        if(error)
            throw std::runtime_error("failed to move blocked file " + filepath.string() + " out of the way ...");
        outputFile.open(filepath, bnw::ofstream::binary | bnw::ofstream::trunc);
    }
    if(!outputFile)
        throw std::runtime_error("Failed to open output file " + filepath.string());

    while(bzerror == BZ_OK)
    {
        std::array<char, 1024> buffer;
        unsigned read = BZ2_bzRead(&bzerror, bz2fp, buffer.data(), static_cast<int>(buffer.size()));
        if(!outputFile.write(buffer.data(), read))
            bnw::cerr << "failed to write to disk" << std::endl;
    }

    bnw::cout << "ok";

    BZ2_bzReadClose(&bzerror, bz2fp);
    fclose(bzfp);

    // remove compressed file
    bfs::remove(bzfile);

    bnw::cout << std::endl;

#ifdef _WIN32
    // \r not working fix
    backslashfix_y = backslashrfix(0);
#endif // !_WIN32
}

/// Copy srcFile to destination or create a symlink at dst pointing to src
void copyOrSymlink(const bfs::path& srcFileName, const bfs::path& dstFilepath)
{
#ifdef _WIN32
    bnw::cout << "Copying file " << srcFileName << std::endl;
    bfs::path path = dstFilepath.parent_path();
    bfs::path srcFilepath = path / srcFileName;
    boost::system::error_code ec;
    bfs::copy_file(srcFilepath, dstFilepath, bfs::copy_option::overwrite_if_exists, ec);
    if(ec)
        bnw::cerr << "Failed to copy file '" << srcFilepath << "' to '" << dstFilepath << "': " << ec.message()
                  << std::endl;
#else
    bnw::cout << "Creating symlink " << dstFilepath << std::endl;
    if(!bfs::exists(dstFilepath))
    {
        boost::system::error_code ec;
        bfs::create_symlink(srcFileName, dstFilepath, ec);
        if(ec)
            bnw::cerr << "Failed to create symlink: '" << dstFilepath << "' to '" << srcFileName
                      << "': " << ec.message() << std::endl;
    }
#endif
}

auto getPossibleHttpBases(const bool nightly)
{
    std::string base = HTTPHOST;
    if(nightly)
        base += NIGHTLYPATH;
    else
        base += STABLEPATH;

    std::stringstream url;
    url << base << TARGET << "." << ARCH;
    const auto archBase = url.str();
    std::vector<std::string> bases = {archBase + FILEPATH};
    for(int i = 1; i <= 5; i++)
    {
        url.str(archBase);
        url << "." << i << FILEPATH;
        bases.push_back(url.str());
    }
    return bases;
}

void executeUpdate(int argc, char* argv[])
{
    bool updated = false;
    bool verbose = false;
    bool nightly = true;
    bfs::path workPath = bfs::path(argv[0]).parent_path().lexically_normal();

    // If the installation is the default one, update current installation
    // TODO: get these paths from cmake and implement some kind of automatic search?
#ifdef _WIN32
    bfs::path tmpPath = workPath.parent_path();
    if(bfs::exists(workPath.parent_path() / std::string("RTTR/s25update.exe")))
        workPath = tmpPath;
#elif defined(__APPLE__)
    bfs::path tmpPath = workPath.parent_path().parent_path().parent_path();
    if(bfs::exists(tmpPath / std::string("s25client.app/Contents/MacOS/s25update")))
        workPath = tmpPath;
#else
    bfs::path tmpPath = workPath.parent_path();
    if(bfs::exists(tmpPath / std::string("libexec/s25rttr/s25update")))
        workPath = tmpPath;
#endif

    if(argc > 1)
    {
        for(int i = 1; i < argc; ++i)
        {
            if(strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
                verbose = true;
            if(strcmp(argv[i], "--dir") == 0 || strcmp(argv[i], "-d") == 0)
                workPath = argv[++i];
            if(strcmp(argv[i], "--stable") == 0 || strcmp(argv[i], "-s") == 0)
                nightly = false;
        }
    }

    if(verbose)
        bnw::cout << "Using directory " << workPath << std::endl;
    boost::system::error_code error;
    bfs::current_path(workPath, error);
    if(error)
        bnw::cerr << "Warning: Failed to set working directory: " << error << std::endl;

    if(!isCurrentDirWritable())
    {
        if(runAsAdmin(argc, argv))
        {
            bnw::cout << "Update should have been run successfully" << std::endl;
            return;
        } else
            throw std::runtime_error("Update failed. Current dir is not writeable");
    }

    // initialize curl
    curl_global_init(CURL_GLOBAL_ALL);
    atexit(curl_global_cleanup);

    // download filelist
    if(verbose)
        bnw::cout << "Requesting current version information from server..." << std::endl;
    std::string httpbase, filelist;
    const auto possibleBases = getPossibleHttpBases(nightly);
    for(size_t i = 0; i < possibleBases.size(); i++)
    {
        auto filelistOpt = DownloadFile(possibleBases[i] + FILELIST);
        if(!filelistOpt)
            bnw::cout << "Warning: Was not able to get masterfile " << i << ", trying older one" << std::endl;
        else
        {
            filelist = *filelistOpt;
            httpbase = possibleBases[i];
            break;
        }
    }
    if(filelist.empty())
        throw std::runtime_error("Could not get any master file");

    // httpbase now includes targetpath and filepath

    // download linklist
    const auto linklist = DownloadFile(httpbase + LINKLIST);
    if(!linklist)
        bnw::cout << "Warning: Was not able to get linkfile, ignoring" << std::endl;

    if(verbose)
        bnw::cout << "Parsing update list..." << std::endl;

    const auto files = parseFileList(filelist);
    const auto itSavegameversion = std::find_if(
      files.begin(), files.end(), [](const auto& it) { return it.second.find(SAVEGAMEVERSION) != std::string::npos; });

    if(itSavegameversion != files.end() && bfs::exists(itSavegameversion->second))
    {
        if(!ValidateSavegameVersion(httpbase, itSavegameversion->second))
            return;
    }

    const auto links = parseLinkList(*linklist);

    // check md5 of files and download them
    for(const auto& file : files)
    {
        const std::string hash = file.first;
        const std::string filePath = file.second;

        if(hash == md5sum(filePath))
            continue;

        updateFile(httpbase, filePath, verbose);
        updated = true;
    }

    if(verbose)
        bnw::cout << "Updating folder structure..." << std::endl;

    for(const auto& link : links)
    {
        // Note: Symlink = first pointing to second (second exists)
        copyOrSymlink(link.second, link.first);
    }

    if(updated)
        bnw::cout << "Update finished!" << std::endl;
}

/**
 *  main function
 */
int main(int argc, char* argv[])
{
    try
    {
        executeUpdate(argc, argv);
    } catch(const std::exception& e)
    {
        bnw::cerr << "Update failed: " << e.what() << std::endl;
        return 1;
    }

#if defined _DEBUG && defined _MSC_VER
    bnw::cout << "Press return to continue . . ." << std::flush;
    bnw::cin.get();
#endif

    return 0;
}
