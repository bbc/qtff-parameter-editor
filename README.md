![BBC RD Logo](./bbcrd-logo.png)

# QuickTime File Format and ProRes Video Parameter Editing

Part of the [HDR-TV](http://www.bbc.co.uk/rd/projects/high-dynamic-range) series. Last updated April 2017.

# Introduction

Several post-production tools and utilities are now aware of the colour and transfer function parameters specified in [ITU-R BT.2100-0](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-0-201607-I!!PDF-E.pdf). However, some tools are unable to correctly signal the correct parameters, and may result in a file with the incorrect video parameters. Subsequent tools or displays may then look at these video parameters and render the image incorrectly, for instance, video that this signalled as [ITU-R BT.709](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf) colour primaries and in fact is [ITU-R BT.2020](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2020-2-201510-I!!PDF-E.pdf) colour primary and then displayed on a monitor will look desaturated when the display is interpreting the signalling contained within the file. Incorrect signalling may also result in unnecessary and incorrect transcoding between colour spaces and transfer functions.  

This document introduces a series of tools to allow editing of the colour primaries, colour matrix and transfer function characteristics in a [QuickTime File Format](https://developer.apple.com/library/content/documentation/QuickTime/QTFF/QTFFPreface/qtffPreface.html) (MOV) using a [ProRes](https://support.apple.com/en-gb/HT202410) video codec.

# QuickTime File Format (qtff)

The [QuickTime File Format](https://developer.apple.com/library/content/documentation/QuickTime/QTFF/QTFFPreface/qtffPreface.html) (qtff) is a container file supporting a wide range of video, audio and other data formats. The format itself is object-orientated, consisting of a collection of objects that can be parsed and expanded.

The basic data unit is known as an Atom. The Atom that defines the relevant information required to define the colour primaries, colour matrix and transfer function are found in the "colr" data Atom, which is located inside the [Video Media](https://developer.apple.com/library/content/documentation/QuickTime/QTFF/QTFFChap3/qtff3.html#//apple_ref/doc/uid/TP40000939-CH205-74522) Atom. The structure of the "colr" Atom is as follows:

| Colour Atom                    | Bytes  |
| -------------------------------| ------ |
| Atom Size                      | 4      |
| Type = "colr"                  | 4      |
| Colour Parameter type = "nclc" | 4      |
| Primary index = 1              | 2      |
| Transfer Function index = 1    | 2      |
| Colour Matrix index = 1        | 2      |

# SMPTE RDD 36:2015 - Apple ProRes Bitstream Syntax and Decoding Process

[SMPTE RDD 36](http://ieeexplore.ieee.org/document/7438722/) describes the syntax and decoding process for the [Apple ProRes](https://support.apple.com/en-gb/HT202410) video compression scheme. It is an intra-frame codec, specifically designed for high-quality workflows and supports a variety of video formats, and is common usage.

In addition to the colour information carried within the Color Atom, information regarding the transfer function, colour matrix and primaries are also stored within the frame header information of the ProRes elementary stream, alongside other parameters, such as frame rate, spatial resolution and chroma format. This header is repeated throughout the bitstream. Full details of the header layout can be found in the [SMPTE specification](http://ieeexplore.ieee.org/document/7438722/).

To avoid any ambiguity in any workflows, it is imperative that the the information in the header for the ProRes stream match that of the qtff colr Atom.

# Video Characteristics

The colour primaries can be selected from the list:

| No.  | Colour Primaries  |
| -----| --------------    |
|0     | Reserved          |
|1     | ITU-R BT.709      |
|2     | Unspecified       |
|3     | Reserved          |
|4     | ITU-R BT.470M     |
|5     | ITU-R BT.470BG    |
|6     | SMPTE 170M        |
|7     | SMPTE 240M        |
|8     | FILM              |
|9     | ITU-R BT.2020     |
|10    | SMPTE ST 428-1    |
|11    | DCI P3            |
|12    | P3 D65            |

The transfer function can be selected from the list:

| No.  | Transfer Function                   |
| -----| ---------------------------------   |
|0     | Reserved                            |
|1     | ITU-R BT.709                        |
|2     | Unspecified                         |
|3     | Reserved                            |
|4     | Gamma 2.2 curve                     |
|5     | Gamma 2.8 curve                     |
|6     | SMPTE 170M                          |
|7     | SMPTE 240M                          |
|8     | Linear                              |
|9     | Log                                 |
|10    | Log Sqrt                            |
|11    | IEC 61966-2-4                       |
|12    | ITU-R BT.1361 Extended Colour Gamut |
|13    | IEC 61966-2-1                       |
|14    | ITU-R BT.2020 10 bit                |
|15    | ITU-R BT.2020 12 bit                |
|16    | SMPTE ST 2084 (PQ)                  |
|17    | SMPTE ST 428-1                      |
|18    | ARIB STD-B67 (HLG)                  |

The colour matrix can be selected from the list:    

| No.  | Colour Matrix                  |
| -----| ---------------------------    |   
|0     |GBR                             |
|1     |BT709                           |
|2     |Unspecified                     |
|3     |Reserved                        |
|4     |FCC                             |
|5     |BT470BG                         |
|6     |SMPTE 170M                      |
|7     |SMPTE 240M                      |
|8     |YCOCG                           |
|9     |BT2020 Non-constant Luminance   |
|10    |BT2020 Constant Luminance       |

# Tools

This repository contains a number of tools that will aid in analysing a video file to obtain the video characteristics, and subsequently allow modification of the qttv container and ProRes bitstream to alter the characteristics

## Getting Started

### Prerequisites

[ffprobe](https://ffmpeg.org/ffprobe.html) is required to find the extract the location of the frame headers from the ProRes bitstream. Downloads can be found [here](https://ffmpeg.org/download.html) for your OS. Static builds for [Linux](https://ffmpeg.org/download.html#build-linux) are available if building from source is not an option. Static builds for Windows and OSX are available via third party websites. Once obtained, ffprobe must be put into the `PATH`.  

[MediaInfo](https://mediaarea.net/en/MediaInfo) is a convenient unified display of the most relevant technical and tag data for video and audio files. The tool is useful checking the outputs to the tools within thsi repository are reporting accurate information.

Standard developer tools (gcc/g++, make, bash) will be required to build and run the code. 

### Building the code

A makefile has been provided to build the code.

```
cd src
make
```
The repository contains 3 main programs:

* `movdump`   - This tool creates a text dump of the header data at the qtff (mov) level.
* `rdd36dump` - This tool creates a text dump of the header data at the ProRes level.
* `rdd36mod` -  This tool modifies the ProRes data to adjust the transfer function, colour primaries and matrix.

### Running the code

The programs above can be run individually as follows:

`movdump ipFile > movdump.txt`

Where the output movdump.txt will look something like this:

```
mdat: s=          5884636816 (0x000000015ec06e90), o=         0 (0x00000000)
  ...skipped 5884636800 bytes
free: s=     24332 (0x00005f0c), o=          5884636816 (0x000000015ec06e90)
  ...skipped 24324 bytes
wide: s=         8 (0x00000008), o=          5884661148 (0x000000015ec0cd9c)
mdat: s=        12 (0x0000000c), o=          5884661156 (0x000000015ec0cda4)
  ...skipped 4 bytes
moov: s=     24938 (0x0000616a), o=          5884661168 (0x000000015ec0cdb0)
    mvhd: s=       108 (0x0000006c), o=          5884661176 (0x000000015ec0cdb8)
      version: 0
      flags: 0x000000
      ...
      ...
```

To run `rdd36dump` and `rdd36mod`, the location of the headers must first be located with `ffprobe`.

`ffprobe -loglevel panic -show_packets -select_streams v:0 ipFile.mov | grep pos > header_offsets.txt`

The header_offsets.txt can then be provided to `rdddump`

`rdd36dump --offsets header_offsets.txt ipFile.mov > rdd36dump.txt`

Where the output rdd36dump.txt will look something like this:

```
frame: num=0, pos=96256
     frame_size: 221808
     frame_identifier: 0x69637066 (icpf)
     frame_header:
         frame_header_size: 148
         reserved: 0x00
         bitstream_version: 0
         encoder_identifier: 0x61626d30 (abm0)
         horizontal_size: 3840
         vertical_size: 2160
         chroma_format: 2 (4:2:2)
         ...
         ...
```

### Modifying the video characteristics

Using the tools above the transfer function, colour primaries and matrix can be edited using the binary offset information in the dump files. Alternatively, a script has been prepared that does it all for you.
The help from the bash script describes its usage:

```

   qtff-parameter-editor.sh

   This script can be used to edit the primaries, transfer functions and matrix
   fields in a .mov file, it can be extended for further editting if required."

    Usage:
            qtff-parameter-editor.sh [--help] [-h] [-p priValue]
                                           [-t tfValue]
                                           [-m matValue]
                                           InputFile
                                           OutputFile
    or
            qtff-parameter-editor.sh InputFile

    Where:
      -h, --help :               Displays this help page
      -p --primaries priValue:   Where priValue is the required primaries value number
      -t --tf tfValue:           Where tfValue is the required transfer function value number
      -m --matrix matValue:      Where matValue is the required matrix function value number
      InputFile:                 Source mov file
      OutputFile:                Output mov file (Can be the same as the Input file)

   If only an Input file is provided (as in the second example, the script will return
   the information for the input file

   Example 1: ./qtff-parameter-editor.sh input.mov
              Return video parameter information for input.mov

   Example 2: ./qtff-parameter-editor.sh -p 1 -t 1 -m 1 input.mov output.mov
              Will create a copy with input.mov and modify the video parameters to bt709
              primaries, transfer function and matrix.

   Example 3: ./qtff-parameter-editor.sh --primaries 9 --tf 18 --matrix 9 input.mov input.mov
              Will modify the video parameters in situ to bt2020 primaries, HLG transfer
              function and bt2020 non constant luminance matrix


```    

If a colr Atom is not present in the video file, the process will not attempt to insert one, and will quit the processing.

# Resources

Please visit our [project page]((http://www.bbc.co.uk/rd/projects/high-dynamic-range) for more information about High Dynamic Range (HDR) and Hybrid Log-Gamma (HLG).

# Authors

This software was written by Philip de Nier and Manish Pindoria (manish.pindoria at bbc.co.uk)

# Contact and Legal Information

Copyright 2009-2017 British Broadcasting Corporation

The qtff-parameter-editor is free software; you can redistribute it and/or modify it under the terms of license agreement.

The qtff-parameter-editor is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
