# PCB

Electronics design files for the Smart Scroll Remote, designed in **EasyEDA**. The design is also published
on OSHWLab, where it can be opened and cloned directly in EasyEDA.

| Path | What |
|---|---|
| [`easyeda/smart-scroll-remote.epro2`](easyeda/smart-scroll-remote.epro2) | EasyEDA Pro project: schematic and PCB in one file (File → Import in EasyEDA Pro) |
| [`fabrication/smart-scroll-remote-gerber.zip`](fabrication/smart-scroll-remote-gerber.zip) | Gerbers, ready to upload to a PCB manufacturer as is |
| [`fabrication/smart-scroll-remote-bom.csv`](fabrication/smart-scroll-remote-bom.csv) | Bill of materials |
| [`fabrication/smart-scroll-remote-cpl.csv`](fabrication/smart-scroll-remote-cpl.csv) | Pick-and-place (CPL) for assembly |
| [`schematic.pdf`](schematic.pdf) | Schematic, for quick viewing on GitHub |
| [`../enclosure/cad/smart-scroll-remote-assembly.step`](../enclosure/cad/smart-scroll-remote-assembly.step) | 3D model: the populated board is included in the enclosure assembly |

The firmware expects the pinout listed in the [main README](../../README.md#pinout).

**Antenna:** keep the module's PCB antenna at the board edge with no copper, ground plane, battery or metal
within about 15 mm. See the note in the main README.

## Licence

Copyright Loic Daigle 2026.

This source describes Open Hardware and is licensed under the CERN-OHL-S v2.

You may redistribute and modify this source and make products using it under the terms of the
[CERN-OHL-S v2](../LICENSE) (https://ohwr.org/cern_ohl_s_v2.txt).

This source is distributed WITHOUT ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING OF MERCHANTABILITY,
SATISFACTORY QUALITY AND FITNESS FOR A PARTICULAR PURPOSE. Please see the CERN-OHL-S v2 for applicable
conditions.

Source location: https://github.com/loic-daigle/smart-scroll-remote

As per CERN-OHL-S v2 section 4, should you produce hardware based on this source, you must where
practicable maintain the Source Location visible on the external case of the Smart Scroll Remote or
other products you make using this source.
