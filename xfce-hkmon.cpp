/*
 * Hacker's Monitor for XFCE Generic Monitor applet
 * Copyright (C) 2015 Ciriaco Garcia de Celis
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

// g++ -std=c++0x -O3 -lrt xfce-hkmon.cpp -o xfce-hkmon (gcc >= 4.6 or clang++)
// Recommended 1 second period and "Bitstream Vera Sans Mono" font on the applet

#include <cstdlib>
#include <memory>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <sstream>
#include <limits>
#include <string>
#include <cctype>
#include <cmath>
#include <ctime>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define APP_VERSION "2.0"

#define VA_STR(x) dynamic_cast<std::ostringstream const&>(std::ostringstream().flush() << x).str()

auto constexpr MB_i = 1000000LL;
auto constexpr MB_f = 1000000.0;
auto constexpr GB_i = 1000000000LL;
auto constexpr GB_f = 1000000000.0;
auto constexpr TB_i = 1000000000000LL;
auto constexpr TB_f = 1000000000000.0; // only C++14 has a readable alternative

void abortApp(const char* reason)
{
    std::cout << "<txt>ERROR " << errno << ":\n" << (reason? reason : "") << "</txt>"
              << "<tool>" << strerror(errno) << "</tool>";
    exit(2);
}

bool readFile(const char* inputFile, std::vector<char>& buffer, bool mustExist = true)
{
    int fd = open(inputFile, O_RDONLY);
    if (fd < 0)
    {
        if (mustExist) abortApp(inputFile);
        return false;
    }
    else
    {
        buffer.resize(4000);
        for (std::size_t offset = 0;;)
        {
            int bytes = ::read(fd, &buffer[offset], buffer.size() - offset - 1);
            if (bytes < 0) abortApp(inputFile);
            offset += bytes;
            if (offset + 1 == buffer.size()) buffer.resize(buffer.size() * 2);
            else if (bytes == 0)
            {
                close(fd);
                buffer[offset] = 0;
                buffer.resize(offset);
                return true;
            }
        }
    }
}

bool writeFile(const char* outputFile, const std::ostringstream& data)
{
    bool success = false;
    int fd = open(outputFile, O_CREAT | O_WRONLY | O_TRUNC, 0640);
    if (fd >= 0)
    {
        const std::string& buffer = data.str();
        if (write(fd, buffer.c_str(), buffer.length()) == (ssize_t) buffer.length()) success = true;
        close(fd);
    }
    return success;
}

template <typename K, typename V> std::ostream& operator<<(std::ostream& out, const std::map<K,V>& container)
{
    for (const auto& item : container) out << item.first << '|' << item.second << '\t'; // write into storage
    return out << '\n';
}

template <typename T> bool fromString(const std::string& from, T& to) { return std::istringstream(from) >> to; }

template <> bool fromString(const std::string& from, std::string& to) { to = from; return true; }

template <typename K, typename V> std::istream& operator>>(std::istream& in, std::map<K,V>& container)
{
    while (!in.eof() && !in.fail() && (in.peek() != '\n')) // read from storage
    {
        std::string skey;
        if (std::getline(in, skey, '|'))
        {
            K key;
            V value;
            if (fromString(skey, key) && (in >> value))
            {
                container.insert( { key, value } );
                in.ignore(); // tab
            }
        }
    }
    return in.ignore();
}

struct CPU
{
    typedef int16_t Number; // -1 for all cores summary and 0,1,2,... for each core

    struct Core
    {
        int64_t user, nice, system, idle, iowait, irq, softirq, steal, guest, guestnice;
        uint64_t freq_hz;
        int64_t cpuUsed()  const { return user + nice + system + irq + softirq + guest + guestnice; }
        int64_t cpuTotal() const { return cpuUsed() + idle + iowait + steal; }
        Core operator-(const Core& that) const
        {
            int64_t roundUserFix = user == that.user-1? 1 : 0; // compensate for physical host & virtual
            int64_t roundNiceFix = nice == that.nice-1? 1 : 0; // guests counters evil rounding
            return
            {
                user - that.user + roundUserFix, nice - that.nice + roundNiceFix, system - that.system,
                idle - that.idle, iowait - that.iowait, irq - that.irq, softirq - that.softirq,
                steal - that.steal, guest - that.guest + roundUserFix, guestnice - that.guestnice + roundNiceFix,
                (freq_hz + that.freq_hz) / 2 // <-- a more useful abstraction
            };
        }
    };

    std::map<Number, Core> cores;

    void readProc()
    {
        std::vector<char> buffer;
        readFile("/proc/stat", buffer);
        std::istringstream cpustat(&buffer[0]);
        std::string name;
        Number number;
        while (cpustat >> name)
        {
            if (name.find("cpu") == 0)
            {
                name.erase(0,3);
                if (name.empty()) number = -1; else if (!fromString(name, number)) continue;
                Core& core = cores[number];
                cpustat >> core.user >> core.nice >> core.system >> core.idle
                        >> core.iowait >> core.irq >> core.softirq;
                if (cpustat.peek() != '\n') cpustat >> core.steal; else core.steal = 0; // caution with old kernels
                if (cpustat.peek() != '\n') cpustat >> core.guest; else core.guest = 0;
                if (cpustat.peek() != '\n') cpustat >> core.guestnice; else core.guestnice = 0;
                core.freq_hz = 0;
                core.user -= core.guest;     // adjust for the already accounted values (may cause evil rounding
                core.nice -= core.guestnice; // effects, though)
            }
            cpustat.ignore(buffer.size(), '\n');
        }
        readFile("/proc/cpuinfo", buffer);
        std::istringstream cpuinfo(&buffer[0]);
        std::string key;
        std::string skip;
        uint64_t sum_freq = 0;
        number = -1;
        while (cpuinfo >> key)
        {
            if (key == "processor") cpuinfo >> skip >> number;
            else if ((key == "cpu") && (number >= 0))
            {
                if ((cpuinfo >> skip) && (skip == "MHz"))
                {
                    double mhz;
                    cpuinfo >> skip >> mhz;
                    cores[number].freq_hz = uint64_t(mhz * MB_i);
                    sum_freq += cores[number].freq_hz;
                    number = -1;
                }
            }
            cpuinfo.ignore(buffer.size(), '\n');
        }
        auto allcpu = cores.find(-1);
        if (allcpu != cores.end()) allcpu->second.freq_hz = sum_freq;
    }
};

std::ostream& operator<<(std::ostream& out, const CPU::Core& core) // write into storage
{
    return out << core.user << ' ' << core.nice << ' ' << core.system << ' ' << core.idle << ' '
               << core.iowait << ' ' << core.irq << ' ' << core.softirq << ' '
               << core.steal << ' ' << core.guest << ' ' << core.guestnice << ' ' << core.freq_hz;
}

std::istream& operator>>(std::istream& in, CPU::Core& core) // read from storage
{
    return in >> core.user >> core.nice >> core.system >> core.idle >> core.iowait >> core.irq
              >> core.softirq >> core.steal >> core.guest >> core.guestnice >> core.freq_hz;
}

struct Memory
{
    struct RAM
    {
        uint64_t total;
        uint64_t available;
        uint64_t free;
        uint64_t shared;
        uint64_t buffers;
        uint64_t cached;
        uint64_t swapTotal;
        uint64_t swapFree;
    };

    RAM ram;

    void readProc()
    {
        std::vector<char> buffer;
        readFile("/proc/meminfo", buffer);
        std::istringstream meminfo(&buffer[0]);
        bool hasAvailable = false;
        std::string key;
        for (int count = 0; meminfo >> key;)
        {
            if      (key == "MemTotal:")     { count++; meminfo >> ram.total;     }
            else if (key == "MemFree:")      { count++; meminfo >> ram.free;      }
            else if (key == "MemAvailable:") { count++; meminfo >> ram.available; hasAvailable = true; }
            else if (key == "Buffers:")      { count++; meminfo >> ram.buffers;   }
            else if (key == "Cached:")       { count++; meminfo >> ram.cached;    }
            else if (key == "SwapTotal:")    { count++; meminfo >> ram.swapTotal; }
            else if (key == "SwapFree:")     { count++; meminfo >> ram.swapFree;  }
            else if (key == "Shmem:")        { count++; meminfo >> ram.shared;    }
            if (count == 7 + (hasAvailable? 1:0)) break;
            meminfo.ignore(buffer.size(), '\n');
        }
        if (!hasAvailable) ram.available = ram.free + ram.buffers + ram.cached; // pre-2014 kernels
    }
};

std::ostream& operator<<(std::ostream& out, const Memory::RAM& ram)
{
    return out << ram.total << ' ' << ram.available << ' ' << ram.free << ' ' << ram.shared << ' '
               << ram.buffers << ' ' << ram.cached << ' ' << ram.swapTotal << ' ' << ram.swapFree << '\n';
}

std::istream& operator>>(std::istream& in, Memory::RAM& ram)
{
    return in >> ram.total >> ram.available >> ram.free >> ram.shared
              >> ram.buffers >> ram.cached >> ram.swapTotal >> ram.swapFree;
}

struct IO
{
    typedef std::string Name;

    struct Device
    {
        uint64_t bytesRead;
        uint64_t bytesWritten;
        uint32_t ioMsecs;
        uint64_t bytesSize;
    };

    struct Bandwidth { double bytesPerSecond; };

    std::map<Name, Device> devices;

    void readProc()
    {
        std::vector<char> buffer;
        readFile("/proc/diskstats", buffer);
        std::string name, prev;
        for (std::istringstream diskinfo(&buffer[0]);;) // search physical devices
        {
            uint64_t skip;
            if (!(diskinfo >> skip >> skip >> name)) break;
            if (!name.empty()
                && (prev.empty() || (name.find(prev) != 0)) // skip partitions
                && (name.find("dm") != 0)) // skip device mapper
            {
                prev = name;
                Device& device = devices[name];
                uint64_t sectorsRd, sectorsWr;
                diskinfo >> skip >> skip >> sectorsRd
                         >> skip >> skip >> skip >> sectorsWr
                         >> skip >> skip >> device.ioMsecs;
                device.bytesRead = sectorsRd*512;
                device.bytesWritten = sectorsWr*512;
                device.bytesSize = 0;
            }
            diskinfo.ignore(buffer.size(), '\n');
        }
        readFile("/proc/partitions", buffer);
        std::istringstream partinfo(&buffer[0]);
        uint64_t blocks;
        std::string skip;
        bool hasData = false;
        while (!hasData && std::getline(partinfo, skip)) if (skip.empty()) hasData = true;
        if (hasData) while (partinfo >> skip >> skip >> blocks >> name)
        {
            auto pdev = devices.find(name);
            if (pdev != devices.end()) pdev->second.bytesSize = blocks * 1024;
            partinfo.ignore(buffer.size(), '\n');
        }
    }
};

std::ostream& operator<<(std::ostream& out, const IO::Device& dev)
{
    return out << dev.bytesRead << ' ' << dev.bytesWritten << ' ' << dev.ioMsecs << ' ' << dev.bytesSize;
}

std::istream& operator>>(std::istream& in, IO::Device& dev)
{
    return in >> dev.bytesRead >> dev.bytesWritten >> dev.ioMsecs >> dev.bytesSize;
}

std::ostream& operator<<(std::ostream& out, const IO::Bandwidth& data)
{
    if (data.bytesPerSecond < MB_i)
        return out << std::fixed << std::setprecision(1) << data.bytesPerSecond / MB_f << " MB/s";
    if (data.bytesPerSecond < GB_i)
        return out << int64_t(data.bytesPerSecond / MB_f) << " MB/s";
    return out << std::fixed << std::setprecision(3) << data.bytesPerSecond / GB_f << " GB/s";
}

struct Network
{
    typedef std::string Name;

    struct Interface
    {
        uint64_t bytesRecv;
        uint64_t bytesSent;
        uint64_t traffic() const { return bytesRecv + bytesSent; }
    };

    struct Bandwidth { int64_t bitsPerSecond; };

    std::map<Name, Interface> interfaces;

    void readProc()
    {
        std::vector<char> buffer;
        readFile("/proc/net/dev", buffer);
        std::istringstream netinfo(&buffer[0]);
        netinfo.ignore(buffer.size(), '\n');
        for (;;)
        {
            netinfo.ignore(buffer.size(), '\n');
            std::string name;
            if (!std::getline(netinfo, name, ':')) break; // also handles kernels not having a space after ':'
            while (!name.empty() && (name.front() == ' ')) name.erase(name.begin()); // ltrim
            Interface& interface = interfaces[name];
            uint64_t skip;
            netinfo >> interface.bytesRecv
                    >> skip >> skip >> skip >> skip >> skip >> skip >> skip
                    >> interface.bytesSent;
        }
    }
};

std::ostream& operator<<(std::ostream& out, const Network::Interface& ifz)
{
    return out << ifz.bytesRecv << ' ' << ifz.bytesSent;
}

std::istream& operator>>(std::istream& in, Network::Interface& ifz)
{
    return in >> ifz.bytesRecv >> ifz.bytesSent;
}

std::ostream& operator<<(std::ostream& out, const Network::Bandwidth& data)
{
    if (data.bitsPerSecond < MB_i) return out << data.bitsPerSecond / 1000 << " Kbps";
    return out << std::fixed << std::setprecision(3) << data.bitsPerSecond / MB_f << " Mbps";
}

struct Health
{
    typedef std::string Name;

    struct Thermometer
    {
        int32_t tempMilliCelsius;
    };

    std::map<Name, Thermometer> thermometers;

    void readProc()
    {
        std::string coretemp;
        for (int hwmon = 0; hwmon < 256; hwmon++)
        {
            std::string base = VA_STR("/sys/class/hwmon/hwmon" << hwmon);
            std::vector<char> buffer;
            if (!readFile(VA_STR(base << "/name").c_str(), buffer, false)) // pre-3.15 kernel?
            {
                base.append("/device");
                if (!readFile(VA_STR(base << "/name").c_str(), buffer, false)) break;
            }
            std::istringstream sname(&buffer[0]);
            std::string name;
            if ((sname >> name) && (name == "coretemp"))
            {
                coretemp = base;
                break;
            }
        }

        if (!coretemp.empty()) for (int ic = 1; ic < 64; ic++)
        {
            std::vector<char> buffer;
            if (!readFile(VA_STR(coretemp << "/temp" << ic << "_label").c_str(), buffer, false))
            {
                if (thermometers.empty()) continue; else break; // Atom CPU may start at 2 (!?)
            }
            std::istringstream sname(&buffer[0]);
            std::string name;
            if (!std::getline(sname, name) || name.empty()) break;
            if (!readFile(VA_STR(coretemp << "/temp" << ic << "_input").c_str(), buffer, false)) break;
            std::istringstream temp(&buffer[0]);
            int32_t tempMilliCelsius;
            if (!(temp >> tempMilliCelsius)) break;
            thermometers[name].tempMilliCelsius = tempMilliCelsius;
        }
    }
};

std::ostream& operator<<(std::ostream& out, const Health::Thermometer& thm)
{
    return out << thm.tempMilliCelsius;
}

std::istream& operator>>(std::istream& in, Health::Thermometer& thm)
{
    return in >> thm.tempMilliCelsius;
}

struct DataSize { uint64_t bytes; };

std::ostream& operator<<(std::ostream& out, const DataSize& data)
{
    if (data.bytes <     5000) return out << "0 MB";
    if (data.bytes <  10*MB_i) return out << std::fixed << std::setprecision(2) << data.bytes / MB_f << " MB";
    if (data.bytes < 100*MB_i) return out << std::fixed << std::setprecision(1) << data.bytes / MB_f << " MB";
    if (data.bytes <     GB_i) return out << data.bytes / MB_i << " MB";
    if (data.bytes <  10*GB_i) return out << std::fixed << std::setprecision(2) << data.bytes / GB_f << " GB";
    if (data.bytes < 100*GB_i) return out << std::fixed << std::setprecision(1) << data.bytes / GB_f << " GB";
    if (data.bytes <     TB_i) return out << data.bytes / GB_i << " GB";
    uint64_t tbx100 = data.bytes / (TB_i / 100);
    auto decimals = (tbx100 > 9999) || (tbx100 % 100 == 0)? 0 : (tbx100 % 10 == 0)? 1 : 2; // pretty disk sizes
    return out << std::fixed << std::setprecision(decimals) << data.bytes / TB_f << " TB";
}

template <typename T> struct Padded { uint64_t max; T value; };

template <typename T> std::ostream& operator<<(std::ostream& out, const Padded<T>& data)
{
    if (!std::isnan(data.value)) for (T div = data.max;; div /= 10) // avoid infinite loop with NaN numbers
    {
        if ((data.value >= div) && (data.value >= 1)) break;
        if ((data.value < 1) && (div <= 1)) break;
        out << "  "; // two spaces in place of each missing digit (same width in XFCE Generic Monitor applet)
    }
    return out << data.value;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
         std::cerr << "usage: " << argv[0] << " [NET|<network_interface>] [CPU] [TEMP] [IO] [RAM]" << std::endl;
         return 1;
    }

    std::shared_ptr<CPU>     new_CPU, old_CPU;
    std::shared_ptr<Memory>  new_Memory;
    std::shared_ptr<IO>      new_IO, old_IO;
    std::shared_ptr<Network> new_Network, old_Network;
    std::shared_ptr<Health>  new_Health;
    std::string selectedNetworkInterface;

    for (int i = 1, match; i < argc; i++)
    {
        std::string arg(argv[i]);
        match = 0;
        if ((arg == "CPU"))  match++, new_CPU.reset(new CPU());
        if ((arg == "RAM"))  match++, new_Memory.reset(new Memory());
        if ((arg == "IO"))   match++, new_IO.reset(new IO());
        if ((arg == "NET"))  match++, new_Network.reset(new Network());
        if ((arg == "TEMP")) match++, new_Health.reset(new Health());
        if (!match)
        {
            new_Network.reset(new Network());
            selectedNetworkInterface = argv[i];
        }
    }

    timespec tp;
    if (clock_gettime(CLOCK_MONOTONIC, &tp) != 0) abortApp("clock_gettime");
    uint64_t nowIs = tp.tv_sec * GB_i + tp.tv_nsec;
    int64_t nsecsElapsed = 0;

    std::ostringstream newState;
    newState << APP_VERSION << " " << nowIs << "\n";
    if (new_CPU)     { new_CPU->readProc();     newState << "CPU|"     << new_CPU->cores;          }
    if (new_Memory)  { new_Memory->readProc();                                                     }
    if (new_IO)      { new_IO->readProc();      newState << "IO|"      << new_IO->devices;         }
    if (new_Network) { new_Network->readProc(); newState << "Network|" << new_Network->interfaces; }
    if (new_Health)  { new_Health->readProc();                                                     }

    for (int locTry = 0;; locTry++) // read the previous state from disk and store the new state
    {
        std::vector<char> oldStateData;
        std::ostringstream stateFileName;

        if (locTry == 0)
            stateFileName << "/run/user/" << getuid() << "/xfce-hkmon.dat";
        else if (locTry == 1)
            stateFileName << "/tmp/xfce-hkmon." << getuid() << ".dat";
        else
            abortApp("can't write tmpfile");

        if (readFile(stateFileName.str().c_str(), oldStateData, false))
        {
            std::istringstream oldState(&oldStateData[0]);
            std::string version;
            uint64_t previouslyWas;
            oldState >> version >> previouslyWas;
            nsecsElapsed = nowIs - previouslyWas;
            oldState.ignore(oldStateData.size(), '\n');
            if (version == APP_VERSION)
            {
                std::string category;
                while (std::getline(oldState, category, '|'))
                {
                    if      (category == "CPU")     { old_CPU.reset(new CPU()); oldState >> old_CPU->cores;  }
                    else if (category == "IO")      { old_IO.reset(new IO());   oldState >> old_IO->devices; }
                    else if (category == "Network")
                    {
                        old_Network.reset(new Network()); oldState >> old_Network->interfaces;
                    }
                    else oldState.ignore(oldStateData.size(), '\n');
                }
            }
        }

        if (writeFile(stateFileName.str().c_str(), newState)) break;
    }

    std::ostringstream reportStd, reportDetail;
    double secsElapsed = nsecsElapsed / GB_f;

    if (new_Network && old_Network && nsecsElapsed) // NET report
    {
        if (selectedNetworkInterface.empty())
        {
            auto highestTraffic = new_Network->interfaces.begin();
            if (highestTraffic != new_Network->interfaces.end())
            {
                for (auto nextIf = highestTraffic; ++nextIf != new_Network->interfaces.end();)
                {
                    if (nextIf->first == "lo") continue;
                    if (nextIf->second.traffic() > highestTraffic->second.traffic()) highestTraffic = nextIf;
                }
                selectedNetworkInterface = highestTraffic->first;
            }
        }

        for (auto itn = new_Network->interfaces.begin(); itn != new_Network->interfaces.end(); ++itn)
        {
            auto ito = old_Network->interfaces.find(itn->first);
            if (ito == old_Network->interfaces.end()) continue;
            const Network::Interface& nif = itn->second;
            const Network::Interface& oif = ito->second;
            bool isSelectedInterface = itn->first == selectedNetworkInterface;
            if (!nif.traffic() && !isSelectedInterface) continue;

            auto dumpNet = [&](const char* iconIdle, const char* iconBusy, uint64_t newBytes, uint64_t oldBytes)
            {
                int64_t delta = newBytes - oldBytes;
                int64_t bps = 8 * delta / secsElapsed;
                const char* icon = delta? iconBusy : iconIdle;
                reportDetail << "    " << icon << "  " << DataSize { newBytes };
                if (bps > 0) reportDetail << " - " << Network::Bandwidth { bps };
                reportDetail << " \n";
                if (isSelectedInterface)
                    reportStd << std::setw(6) << Network::Bandwidth { bps } << " " << icon << " \n";
            };

            reportDetail << " " << itn->first << ": ";
            if (isSelectedInterface) reportDetail << "\u2713"; // "check mark" character
            reportDetail << "\n";
            dumpNet("\u25B3", "\u25B2", nif.bytesSent, oif.bytesSent); // white/black up pointing triangles
            dumpNet("\u25BD", "\u25BC", nif.bytesRecv, oif.bytesRecv); // down pointing triangles
        }
    }

    if (new_CPU && old_CPU) // CPU report
    {
        struct CpuStat { CPU::Number number; double percent; double ghz; };
        std::multimap<double, CpuStat> rankByGhzUsage;
        double cum_weighted_ghz = 0;
        for (auto itn = new_CPU->cores.begin(); itn != new_CPU->cores.end(); ++itn)
        {
            if (itn->first < 0) continue;
            auto ito = old_CPU->cores.find(itn->first);
            if (ito == old_CPU->cores.end()) continue;
            CPU::Core diff = itn->second - ito->second;
            if (diff.cpuTotal() == 0) continue;
            double unityUsage = 1.0 * diff.cpuUsed() / diff.cpuTotal();
            double ghz = diff.freq_hz / GB_f;
            double ghzUsage = ghz * unityUsage;
            cum_weighted_ghz += ghzUsage;
            rankByGhzUsage.insert({ ghzUsage, CpuStat { itn->first, 100.0 * unityUsage, ghz } });
        }

        auto allnew = new_CPU->cores.find(-1);
        auto allold = old_CPU->cores.find(-1);
        if ((allnew != new_CPU->cores.end()) && (allold != old_CPU->cores.end()))
        {
            const CPU::Core& ncpu = allnew->second;
            const CPU::Core& ocpu = allold->second;
            CPU::Core diff = ncpu - ocpu;
            auto cpuTotal = diff.cpuTotal();
            if (cpuTotal > 0)
            {
                auto cpuTotalSinceBoot = ncpu.cpuTotal();
                double usagePercent = 100.0 * diff.cpuUsed() / cpuTotal;

                auto dumpPercent = [&](const char* title, int64_t user_hz, int64_t user_hz__sinceBoot)
                {
                    reportDetail << "   " << Padded<double> { 100, 100.0 * user_hz / cpuTotal } << "% " << title
                                 << "  (" << 100.0 * user_hz__sinceBoot / cpuTotalSinceBoot << "%) \n";
                };

                reportStd << std::setw(6) << std::fixed << std::setprecision(1) << usagePercent << "%";

                reportDetail << " CPU \u2699 " << std::fixed << std::setprecision(2) << usagePercent << "% \u2248 ";

                if (cum_weighted_ghz < 1)
                    reportDetail << uint64_t(cum_weighted_ghz * 1000) << " MHz:\n" << std::setprecision(2);
                else
                    reportDetail << std::setprecision(1) << cum_weighted_ghz << " GHz:\n" << std::setprecision(2);

                dumpPercent("user",   diff.user,   ncpu.user);
                dumpPercent("nice",   diff.nice,   ncpu.nice);
                dumpPercent("system", diff.system, ncpu.system);
                dumpPercent("idle",   diff.idle,   ncpu.idle);
                dumpPercent("iowait", diff.iowait, ncpu.iowait);
                if (ncpu.irq)       dumpPercent("irq",        diff.irq,       ncpu.irq);
                if (ncpu.softirq)   dumpPercent("softirq",    diff.softirq,   ncpu.softirq);
                if (ncpu.steal)     dumpPercent("steal",      diff.steal,     ncpu.steal);
                if (ncpu.guest)     dumpPercent("guest",      diff.guest,     ncpu.guest);
                if (ncpu.guestnice) dumpPercent("guest nice", diff.guestnice, ncpu.guestnice);

                int maxCpu = 8;
                for (auto itc = rankByGhzUsage.rbegin(); maxCpu-- && (itc != rankByGhzUsage.rend()); ++itc)
                {
                    reportDetail << "   " << std::fixed
                        << std::setprecision(2) << Padded<double> { 100, itc->second.percent } << "% cpu "
                        << Padded<CPU::Number> { uint64_t(new_CPU->cores.size() > 10? 10 : 1), itc->second.number }
                        << "  @" << std::setprecision(3) << Padded<double> { 10, itc->second.ghz } << " GHz \n";
                }
            }
        }
    }

    if (new_Memory) // RAM report
    {
        if (!new_Health && new_CPU) reportStd << " " << new_Memory->ram.available/1024 << "M\n";

        reportDetail << " Memory " << new_Memory->ram.total/1024 << " MiB:\n"
            << Padded<uint64_t> { 1000000, new_Memory->ram.available/1024 } << " MiB available \n"
            << Padded<uint64_t> { 1000000, (new_Memory->ram.cached+new_Memory->ram.buffers)/1024 }
            << " MiB cache/buff \n";

        if (new_Memory->ram.shared)
            reportDetail << Padded<uint64_t> { 1000000, new_Memory->ram.shared/1024 } << " MiB shared \n";

        if (new_Memory->ram.swapTotal)
            reportDetail << Padded<uint64_t> { 1000000, (new_Memory->ram.swapTotal-new_Memory->ram.swapFree)/1024 }
                         << " MiB swap of " << new_Memory->ram.swapTotal/1024 << " \n";
    }

    if (new_IO && old_IO && nsecsElapsed) // IO report
    {
        for (auto nitd = new_IO->devices.begin(); nitd != new_IO->devices.end(); ++nitd)
        {
            const IO::Device& device = nitd->second;
            auto prevdev = old_IO->devices.find(nitd->first);
            if ((device.bytesRead || device.bytesWritten) && (prevdev != old_IO->devices.end()))
            {
                reportDetail << " " << nitd->first << " \u26C1 " << DataSize { device.bytesSize } << ":\n";

                auto dumpIO = [&](const char* iconIdle, const char* iconBusy, uint64_t newBytes, uint64_t oldBytes)
                {
                    auto transferred = newBytes - oldBytes;
                    reportDetail << "    " << (transferred? iconBusy : iconIdle) << "  " << DataSize { newBytes };
                    if (transferred) reportDetail << " - " << IO::Bandwidth { transferred / secsElapsed };
                    reportDetail << " \n";
                };

                dumpIO("\u25B3", "\u25B2", device.bytesWritten, prevdev->second.bytesWritten);
                dumpIO("\u25BD", "\u25BC", device.bytesRead, prevdev->second.bytesRead);
            }
        }
    }

    if (new_Health) // TEMP report
    {
        struct ThermalStat
        {
           ThermalStat() : min(std::numeric_limits<int32_t>::max()),
                           max(std::numeric_limits<int32_t>::min()),
                           avg(0), count(0)
           {}
           int32_t min;
           int32_t max;
           int32_t avg;
           std::size_t count;
           Health::Name firstName;
        };

        std::map<std::string, ThermalStat> statByCategory;
        int32_t maxAbsTemp = std::numeric_limits<int32_t>::min();
        for (const auto& itt : new_Health->thermometers)
        {
            std::string key = itt.first;
            auto catEnd = key.find(" ");
            if (catEnd != std::string::npos) key.erase(catEnd);
            auto its = statByCategory.find(key);
            if (its == statByCategory.end())
            {
                its = statByCategory.insert( { key, ThermalStat() } ).first;
                its->second.firstName = itt.first;
            }
            if (itt.second.tempMilliCelsius < its->second.min) its->second.min = itt.second.tempMilliCelsius;
            if (itt.second.tempMilliCelsius > its->second.max) its->second.max = itt.second.tempMilliCelsius;
            if (itt.second.tempMilliCelsius > maxAbsTemp) maxAbsTemp = itt.second.tempMilliCelsius;
            auto prevTotal = its->second.avg * its->second.count;
            its->second.count++;
            its->second.avg = (prevTotal + itt.second.tempMilliCelsius) / its->second.count;
        }

        if (maxAbsTemp >= 0) reportStd << std::setw(4) << maxAbsTemp / 1000 << "ºC\n";

        if (!statByCategory.empty()) reportDetail << " Temperature: \n";

        for (auto its = statByCategory.rbegin(); its != statByCategory.rend(); ++its)
        {
            if (its->second.count == 1)
                reportDetail << "    " << its->second.firstName << ": " << its->second.max / 1000 << "ºC \n";
            else
                reportDetail << "    \u2206" << its->second.max / 1000
                             << "ºC  \u2207" << its->second.min / 1000
                             << "ºC  \u222B" << its->second.avg / 1000
                             << "ºC  (" << its->second.count << " " << its->first << ") \n";
        }
    }

    std::string sReportStd = reportStd.str();
    if (!sReportStd.empty() && (sReportStd.back() == '\n')) sReportStd.erase(sReportStd.end()-1);

    if (sReportStd.empty()) sReportStd = "Hacker's\nMonitor"; // dummy message (allow the user to right-click)

    std::string sReportDetail = reportDetail.str();
    if (!sReportDetail.empty() && (sReportDetail.back() == '\n')) sReportDetail.erase(sReportDetail.end()-1);

    std::cout << "<txt>" << sReportStd << "</txt><tool>" << sReportDetail << "</tool>";
    return 0;
}
