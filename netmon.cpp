/*
 * Copyright (C) 2010 Ciriaco Garcia de Celis
 *
 * This program is  free software:  you can redistribute it and/or
 * modify it under the terms of the  GNU General Public License as
 * published by the Free Software Foundation,  either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY;  without even the implied warranty of
 * MERCHANTABILITY  or  FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License (GPL) for more details.
 *
 * You should have received a copy of the GNU  GPL along with this
 * program. If not, see <http://www.gnu.org/licenses/>.
 */

// compile with "g++ -O3 -lrt netmon.cpp -o netmon"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <climits>

#define STATE_FILE_BASE "/dev/shm/netmon"

int exitapp(int exitcode)
{
    char errmsg[64];
    sprintf(errmsg, "ERROR code %d", exitcode);
    write(1, errmsg, strlen(errmsg));
    return exitcode;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
         printf("usage: %s <network_interface> [CPU] [TEMP]\n", argv[0]);
         return 1;
    }

    bool reportCPU = false;
    bool reportTEMP = false;
    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "CPU")) reportCPU = true;
        if (!strcmp(argv[i], "TEMP")) reportTEMP = true;
    }

    char buffer[4096], cad[256], *ni, *nf;

    // read network information
    int fd = open("/proc/net/dev", O_RDONLY);
    if (fd < 0) return exitapp(2);
    int bytes = read(fd, buffer, sizeof(buffer)-1);
    close(fd);
    if (bytes < 0) return exitapp(3);
    buffer[bytes] = 0;

    timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    long long nanoseconds = tp.tv_sec * 1000000000LL + tp.tv_nsec;

    long long recv_bytes=LLONG_MAX, sent_bytes=LLONG_MAX;
    bool networkAvailable = false;

    // search for the proper network interface
    strcpy(cad, argv[1]);
    strcat(cad, ":");
    char *pif = strstr(buffer, cad);
    if (pif != NULL)
    {
        networkAvailable = true;

        // jump to the received bytes field
        ni = pif + strlen(cad);
        while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
        for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
        *nf++ = 0;

        // get the received bytes
        recv_bytes = atoll(ni);

        // jump to the sent bytes field
        for (int skip = 0; skip < 8; skip++)
        {
            ni = nf;
            while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
            for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
            if (!*nf) break;
            *nf++ = 0;
        }

        // get the sent bytes
        sent_bytes = atoll(ni);
    }

    long long user_mode_time=0, user_mode_nice_time=0, system_mode_time=0, idle_time=0, ioirq_time=0, virtual_time=0;

    if (reportCPU)
    {
        // read CPU information
        fd = open("/proc/stat", O_RDONLY);
        if (fd < 0) return exitapp(4);
        bytes = read(fd, buffer, sizeof(buffer)-1);
        close(fd);
        if (bytes < 0) return exitapp(5);
        buffer[bytes] = 0;

        pif = strstr(buffer, "cpu ");
        if (pif != NULL)
        {
            ni = pif + 3;
            while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
            for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
            *nf++ = 0;

            // get the user mode time
            user_mode_time = atoll(ni);

            ni = nf;
            while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
            for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
            *nf++ = 0;

            // get the user mode nice time
            user_mode_nice_time = atoll(ni);

            ni = nf;
            while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
            for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
            *nf++ = 0;

            // get the system mode time
            system_mode_time = atoll(ni);

            ni = nf;
            while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
            for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
            *nf++ = 0;

            // get the idle time
            idle_time = atoll(ni);

            for (int rest = 0; rest < 3; rest++)
            {
                ni = nf;
                while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
                for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
                *nf++ = 0;

                // get the iowait & irq time
                ioirq_time += atoll(ni);
            }

            for (int rest = 0; rest < 3; rest++)
            {
                ni = nf;
                while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
                for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
                *nf++ = 0;

                // get the virtual time
                virtual_time += atoll(ni);
            }
        }
    }

    int MAXTC = 64;
    long temp[MAXTC];
    long tcores = 0;

    if (reportTEMP)
    {
        int hwmon;
        for (hwmon = 0; hwmon < MAXTC; hwmon++) // required since kernel 3.15
        {
            sprintf(cad, "/sys/class/hwmon/hwmon%d/name", hwmon);
            fd = open(cad, O_RDONLY);
            if (fd < 0) { hwmon = MAXTC; break; }
            bytes = read(fd, buffer, sizeof(buffer)-1);
            close(fd);
            if (bytes < 0) return exitapp(6);
            buffer[bytes] = 0;
            if (!strncmp(buffer, "coretemp\n", 9)) break;
        }

        if (hwmon < MAXTC) for (int ic = 1; ic < MAXTC; ic++)
        {
            sprintf(cad, "/sys/class/hwmon/hwmon%d/temp%d_input", hwmon, ic);
            fd = open(cad, O_RDONLY);
            if (fd < 0) break;
            bytes = read(fd, buffer, sizeof(buffer)-1);
            close(fd);
            if (bytes < 0) return exitapp(7);
            buffer[bytes] = 0;
            temp[ic-1] = atol(buffer);
            tcores++;
        }
    }

    // read the received/sent bytes, date and CPU usage stored by a previous execution
    sprintf(cad, "%s.%s.%d", STATE_FILE_BASE, argv[1], getuid());
    fd = open(cad, O_RDWR | O_CREAT, 0664);
    if (fd < 0) return exitapp(8);
    bytes = read(fd, buffer, sizeof(buffer)-1);
    if (bytes < 0)
    {
        close(fd);
        return exitapp(9);
    }
    long long prev_recv_bytes, prev_sent_bytes, prev_nanoseconds = -1;
    long long prev_user_mode_time, prev_user_mode_nice_time, prev_system_mode_time, prev_idle_time = -1, prev_ioirq_time = -1, prev_virtual_time = -1;
    if (bytes > 0)
    {
        prev_recv_bytes = atoll(buffer);
        prev_sent_bytes = atoll(buffer+20);
        prev_nanoseconds = atoll(buffer+40);
        prev_user_mode_time = atoll(buffer+60);
        prev_user_mode_nice_time = atoll(buffer+80);
        prev_system_mode_time = atoll(buffer+100);
        prev_idle_time = atoll(buffer+120);
        prev_ioirq_time = atoll(buffer+140);
        prev_virtual_time = atoll(buffer+160);
    }

    // store in the file the current values for later use
    sprintf(buffer, "%019lld\n%019lld\n%019lld\n%019lld\n%019lld\n%019lld\n%019lld\n%019lld\n%019lld\n", 
        recv_bytes, sent_bytes, nanoseconds,
        user_mode_time, user_mode_nice_time, system_mode_time, idle_time, ioirq_time, virtual_time);
    lseek(fd, 0, SEEK_SET);
    write(fd, buffer, 180);
    close(fd);

    // generate the result

    strcpy(buffer, "<txt>");

    bool hasNet = networkAvailable && (prev_nanoseconds >= 0) && (recv_bytes >= prev_recv_bytes) && (sent_bytes >= prev_sent_bytes);

    if (!networkAvailable)
    {
        sprintf(cad, "  %s is down", argv[1]);
        strcat(buffer, cad);
    }
    else if (!hasNet)
    {
        strcat(buffer, "     ? kbps IN \n     ? kbps OUT");
    }
    else
    {
        long long elapsed = nanoseconds - prev_nanoseconds;
        if (elapsed < 1) elapsed = 1;
        double seconds = elapsed / 1000000000.0;
        long long sent = sent_bytes - prev_sent_bytes;
        long long received = recv_bytes - prev_recv_bytes;
        long inbps = (long) (8 * received / seconds + 999); // adding 999 ensures "1" for any rate above 0
        long outbps = (long) (8 * sent / seconds + 999);
        if (inbps < 1000000)
            sprintf(cad, "%6d kbps IN \n", inbps/1000);
        else
            sprintf(cad, "%6.3f Mbps IN \n", inbps/1000000.0);
        strcat(buffer, cad);

        if (outbps < 1000000)
            sprintf(cad, "%6d kbps OUT", outbps/1000);
        else
            sprintf(cad, "%6.3f Mbps OUT", outbps/1000000.0);
        strcat(buffer, cad);
        
    }

    if (reportCPU || reportTEMP) strcat(buffer, "\n   ");

    long long cpu_user_used = user_mode_time - prev_user_mode_time;
    long long cpu_usernice_used = user_mode_nice_time - prev_user_mode_nice_time;
    long long cpu_system_used = system_mode_time - prev_system_mode_time;
    long long cpu_ioirq_used = ioirq_time - prev_ioirq_time;
    long long cpu_virtual_used = virtual_time - prev_virtual_time;
    long long cpu_used = cpu_user_used + cpu_usernice_used + cpu_system_used;
    long long total_cpu = cpu_used + (idle_time - prev_idle_time) + cpu_ioirq_used + cpu_virtual_used;
    bool hasCPU = (prev_idle_time >= 0) && (total_cpu > 0);
    if (reportCPU)
    {
        if (!hasCPU)
        {
            strcat(buffer, "  ?  % ");
        }
        else
        {
            sprintf(cad, "%5.1f%% ", cpu_used * 100.0 / total_cpu);
            strcat(buffer, cad);
        }
        if (!reportTEMP) strcat(buffer, "CPU");
    }

    if (reportTEMP && (tcores > 0))
    {
        if (!reportCPU) strcat(buffer, "   ");
        sprintf(cad, " %dºC", temp[0]/1000);
        strcat(buffer, cad);
    }

    strcat(buffer, "</txt><tool>");

    if (networkAvailable && hasNet)
    {
        sprintf(cad, " %s:\n    %.2f MB received \n    %.2f MB sent ",
                    argv[1], recv_bytes/1000000.0, sent_bytes/1000000.0);
        strcat(buffer, cad);
    }

    if (reportCPU && hasCPU)
    {
        if (networkAvailable && hasNet) strcat(buffer, "\n");
        long long total_used_cpu = user_mode_time + user_mode_nice_time + system_mode_time;
        sprintf(cad, " CPU:\n   %5.1f%% user \n",
                    cpu_user_used * 100.0 / total_cpu);
        strcat(buffer, cad);
        sprintf(cad, "   %5.1f%% nice \n",
                    cpu_usernice_used * 100.0 / total_cpu);
        strcat(buffer, cad);
        sprintf(cad, "   %5.1f%% system \n",
                    cpu_system_used * 100.0 / total_cpu);
        strcat(buffer, cad);
        sprintf(cad, "   %5.1f%% wait+irq \n",
                    cpu_ioirq_used * 100.0 / total_cpu);
        strcat(buffer, cad);
        sprintf(cad, "   %5.1f%% virtual OS \n",
                    cpu_virtual_used * 100.0 / total_cpu);
        strcat(buffer, cad);
        sprintf(cad, "   %5.1f%% total since boot ",
                    total_used_cpu * 100.0 / (total_used_cpu + idle_time + ioirq_time));
        strcat(buffer, cad);
    }

    if (reportTEMP & (tcores > 1))
    {
        sprintf(cad,"\n Temperature %dºC:", temp[0]/1000);
        strcat(buffer, cad);
        for (int i = 1; i < tcores; i++)
        {
            sprintf(cad, "\n    Core %d: %dºC", i-1, temp[i]/1000);
            strcat(buffer, cad);
        }
    }

    strcat(buffer, "</tool>");

    write(1, buffer, strlen(buffer));

    return 0;
}
