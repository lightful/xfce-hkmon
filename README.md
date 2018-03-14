## Hacker's Monitor for XFCE

### Screenshot and doc

See the project [web page](https://lightful.github.io/xfce-hkmon/).

### Installation

1. Download [xfce-hkmon.cpp](xfce-hkmon.cpp) and compile it (you only need gcc or clang installed):
```bash
g++ -std=c++0x -O3 -lrt xfce-hkmon.cpp -o xfce-hkmon
```
2. Place the executable somewhere (e.g. /usr/local/bin)
3. Add a XFCE Generic Monitor Applet (comes with most distros) with these settings: no label, 1 second period, *Bitstream Vera Sans Mono* font (recommended) and the following command:
```
/usr/local/bin/xfce-hkmon NET CPU TEMP IO RAM
```
