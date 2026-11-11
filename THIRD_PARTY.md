# Third-party notices and artwork provenance

Logi Options Open is an independent, experimental third-party project. It is
not Logitech Options+, and is not affiliated with, endorsed by, or supported
by Logitech. Logitech, Logi, MX Master and related product names identify the
devices with which interoperability is attempted. They remain their respective
owners' trademarks. The project name does not indicate official status.

## Project code and artwork

The project-authored source and generated schematic artwork in this repository
are provided under the root MIT license. `tools/generate-assets.py` reproduces
the PNG and ICO assets using only the Python standard library. It reads no
external photographs, fonts or logos. The mouse is an original schematic,
not an official product render. Its pin locations illustrate available controls.

The legacy resource filenames `firefox.png`, `chrome.png` and `wps.png` contain
generic browser/document symbols, not those applications' official logos.
No original Options+ product photographs, application cache, proprietary icon
font, executable, firmware, decompiled source, or reverse-engineering dump is
included. The Windows UI can use icons supplied by locally installed programs
at runtime; these are not redistributed as project assets.

## Build and runtime dependencies

- Linux links the system HIDAPI hidraw library. HIDAPI offers GPLv3, BSD-style,
  or its original license at the user's choice. It is not vendored here; see
  <https://github.com/libusb/hidapi/blob/master/LICENSE.txt> for its terms.
- Windows builds link Windows SDK system libraries, including HID, SetupAPI,
  GDI+ and the native controls API. These are supplied by Windows, not relicensed
  under this project's MIT license.
- The native Linux UI uses Qt 6 Widgets. Qt is an external dependency available
  under its own commercial and open-source terms, including LGPL/GPL options.
  This repository does not vendor Qt. A distributor of binaries must follow the
  terms of the particular Qt libraries they distribute; the project license
  does not replace those terms. See <https://www.qt.io/licensing/>.
- API/MCP integration uses Python's standard library. The optional artwork
  generator also uses only the standard library.

Compiler runtimes and any additional libraries linked by a particular Linux
build retain their own licenses. Consult that build's dependency list when
redistributing binaries. System dependencies are not covered by a claim that
all installed software is MIT-licensed.
